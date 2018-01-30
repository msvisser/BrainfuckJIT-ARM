#ifndef __arm__
#error BrainfuckJIT currently only supports ARM
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

/* Buffer size settings, these influence the runtime memory footprint */
#define INPUT_BUFFER_SIZE 512
#define LOOP_STACK_SIZE 100
#define JIT_MEMORY_SIZE 30000

/* Function types for the RLE callback and the actual JIT function */
typedef int (*rle_callback_t)(unsigned char, unsigned int, void *);
typedef void (*jit_function_t)(void);

/* Structure for passing the code generation data to the RLE function */
typedef struct {
    unsigned int **code_memory_pointer;
    unsigned int **loop_stack;
    unsigned int loop_size;
    unsigned int loop_max_size;
} codegen_param_t;

/* Structure for passing flags to the runtime */
typedef struct {
    unsigned int verbose;
    unsigned int jit_memory_size;
    unsigned int loop_stack_size;
    char *input_file_string;
} runtime_flags_t;

/* Read all characters from a file and call the handle_char callback for every
   sequence of equal characters, effectively RLEing the input file */
int rle_read_file(FILE *file, rle_callback_t handle_char, void *param) {
    unsigned char *input_buffer;
    unsigned int read_length;
    unsigned char last_character, this_character;
    unsigned int last_count;
    unsigned int i;
    int return_value;

    /* Allocate a buffer for the input */
    input_buffer = malloc(INPUT_BUFFER_SIZE);
    if (input_buffer == NULL) {
        fprintf(stderr, "Unable to allocate memory for the input buffer\n");
        return 1;
    }

    /* Set up the RLE variables */
    last_character = '\0';
    last_count = 0;

    do {
        /* Read a max of INPUT_BUFFER_SIZE from the input file */
        read_length = fread(input_buffer, sizeof(unsigned char), INPUT_BUFFER_SIZE, file);
        /* Iterate over all characters that were read */
        for (i = 0; i < read_length; i++) {
            this_character = *(input_buffer + i);

            /* If the character is different from the last character, call the
               callback and set the new character */
            if (this_character != last_character) {
                return_value = handle_char(last_character, last_count, param);
                /* If the callback returned an error state stop reading */
                if (return_value != 0) {
                    free(input_buffer);
                    return return_value;
                }
                last_character = this_character;
                last_count = 1;
            } else {
                last_count++;
            }
        }
    } while (read_length != 0);
    /* Finish up final characters */
    if (last_count > 0) {
        return_value = handle_char(last_character, last_count, param);
        if (return_value != 0) {
            free(input_buffer);
            return return_value;
        }
    }

    /* Deallocate the input buffer */
    free(input_buffer);
    return 0;
}

/* Callback function for rle_read_file which will calculate the total number of instructions
   this program will take once compiled */
int rle_determine_code_length(unsigned char character, unsigned int count, void *param) {
    unsigned int *code_length = (unsigned int *) param;
    /* Add number of bytes to the parameter base on character and count */
    switch (character) {
        /* "+-><" can be repeated in the same instruction */
        case '+':
        case '-':
            *code_length += 1 * sizeof(unsigned int);
            break;
        case '>':
        case '<':
            *code_length += 2 * sizeof(unsigned int);
            break;
        /* "[].," need to be repeated for every character */
        case '[':
        case ']':
            *code_length += 2 * count * sizeof(unsigned int);
            break;
        case '.':
        case ',':
            *code_length += (4 + (2 * count)) * sizeof(unsigned int);
            break;
    }

    return 0;
}

/* Callback function for rle_read_file which will actually do the code generation on the file,
   this function requires an instance of codegen_param_t as input parameter */
int rle_code_generate(unsigned char character, unsigned int count, void *param) {
    codegen_param_t *codegen_param = (codegen_param_t *) param;
    unsigned int **code_memory_pointer = codegen_param->code_memory_pointer;
    unsigned int *code_memory = *code_memory_pointer;
    unsigned int i;

    /* Add number of bytes to the parameter base on character and count */
    switch (character) {
        /* "+-><" can be repeated in the same instruction */
        case '+':
            *(code_memory + 0) = 0xe2800000 | (count & 0xff); /* add r0, r0, count */
            *code_memory_pointer += 1;
            break;
        case '-':
            *(code_memory + 0) = 0xe2400000 | (count & 0xff); /* sub r0, r0, count */
            *code_memory_pointer += 1;
            break;
        case '>':
            if (count > 0xfff) {
                fprintf(stderr, "Move right count is too large (> 4095)\n");
                return 2;
            }
            *(code_memory + 0) = 0xe4c10000 | (count & 0xfff); /* strb r0, [r1], count */
            *(code_memory + 1) = 0xe5d10000; /* ldrb r0, [r1] */
            *code_memory_pointer += 2;
            break;
        case '<':
            if (count > 0xfff) {
                fprintf(stderr, "Move left count is too large (> 4095)\n");
                return 2;
            }
            *(code_memory + 0) = 0xe4410000 | (count & 0xfff); /* strb r0, [r1], -count */
            *(code_memory + 1) = 0xe5d10000; /* ldrb r0, [r1] */
            *code_memory_pointer += 2;
            break;
        /* "[].," need to be repeated for every character */
        case '[':
            for (i = 0; i < count; i++) {
                *(code_memory + (i * 2) + 0) = 0xe31000ff; /* tst r0, #255 */
                *(code_memory + (i * 2) + 1) = 0x0a000000; /* beq offset */

                /* Save the memory address of the branch instruction on the stack, so we
                   can fix the offset when we find the closing bracket */
                if (codegen_param->loop_size >= codegen_param->loop_max_size) {
                    fprintf(stderr, "Loop stack size exceeded, try running with a larger -l.\n");
                    return 3;
                }
                *(codegen_param->loop_stack + codegen_param->loop_size++) = (code_memory + (i * 2) + 1);
            }
            *code_memory_pointer += 2 * count;
            break;
        case ']':
            for (i = 0; i < count; i++) {
                unsigned int *back_addr, *cur_addr;
                int back_offset, forward_offset;
                int back_in_range, forward_in_range;
                /* Get the location of the opening bracket for this closing bracket */
                if (codegen_param->loop_size == 0) {
                    fprintf(stderr, "Closing a loop while there is no open loop.\n");
                    return 3;
                }
                back_addr = *(codegen_param->loop_stack + --codegen_param->loop_size);
                cur_addr = (code_memory + (i * 2) + 1);

                /* Determine the branching offsets for the two branch instructions of this bracket pair */
                back_offset = (int)((unsigned int) back_addr - (unsigned int) cur_addr - 4) >> 2;
                forward_offset = (int)((unsigned int) cur_addr - (unsigned int) back_addr - 4) >> 2;

                back_in_range = back_offset >= -0x800000 && back_offset <= 0x7fffff;
                forward_in_range = forward_offset >= -0x800000 && forward_offset <= 0x7fffff;
                if (!back_in_range || !forward_in_range) {
                    fprintf(stderr, "Loop jump requires offset outside of the 32MB jump range.\n");
                    return 3;
                }

                *(code_memory + (i * 2) + 0) = 0xe31000ff; /* tst r0, #255 */
                *(code_memory + (i * 2) + 1) = 0x1a000000 | (back_offset & 0xffffff); /* bne offset */
                *((unsigned int *) back_addr) |= (forward_offset & 0xffffff);
            }
            *code_memory_pointer += 2 * count;
            break;
        case '.':
            *(code_memory + 0) = 0xe5c10000; /* strb r0, [r1] */
            *(code_memory + 1) = 0xe3a07004; /* mov r7, #4 */
            *(code_memory + 2) = 0xe3a02001; /* mov r2, #1 */
            for (i = 0; i < count; i++) {
                /* Run a systemcall write(stdout, jit_memory[current_cell], 1) */
                *(code_memory + 3 + (i * 2) + 0) = 0xe3a00001; /* mov r0, #1 */
                *(code_memory + 3 + (i * 2) + 1) = 0xef000000; /* svc #0 */
            }
            *(code_memory + 3 + (i * 2) + 0) = 0xe5d10000; /* ldrb r0, [r1] */
            *code_memory_pointer += 4 + (2 * count);
            break;
        case ',':
            *(code_memory + 0) = 0xe5c10000; /* strb r0, [r1] */
            *(code_memory + 1) = 0xe3a07003; /* mov r7, #3 */
            *(code_memory + 2) = 0xe3a02001; /* mov r2, #1 */
            for (i = 0; i < count; i++) {
                /* Run a systemcall read(stdin, jit_memory[current_cell], 1) */
                *(code_memory + 3 + (i * 2) + 0) = 0xe3a00000; /* mov r0, #0 */
                *(code_memory + 3 + (i * 2) + 1) = 0xef000000; /* svc #0 */
            }
            *(code_memory + 3 + (i * 2) + 0) = 0xe5d10000; /* ldrb r0, [r1] */
            *code_memory_pointer += 4 + (2 * count);
            break;
    }

    return 0;
}

int run_jit(runtime_flags_t *flags) {
    /* Input variables */
    FILE *input_file;
    int rle_return_value;
    /* JIT variables */
    unsigned int code_length;
    unsigned int *code_memory;
    unsigned int *code_memory_pointer;
    codegen_param_t codegen_param;
    unsigned char *jit_memory;
    jit_function_t jit_function;

    /* Open the input file */
    input_file = fopen(flags->input_file_string, "r");
    if (input_file == NULL) {
        fprintf(stderr, "Could not open file\n");
        return 1;
    }

    /* Reset code length */
    code_length = 0;

    if (flags->verbose) fprintf(stderr, "Determining the output code length\n");
    /* Read the input and determine the code size */
    rle_return_value = rle_read_file(input_file, rle_determine_code_length, &code_length);
    /* Check if there was an error determining the code length */
    if (rle_return_value != 0) {
        /* Clean up used memory */
        fclose(input_file);
        return rle_return_value;
    }

    /* Increase the code length to allow for pre- and postamble */
    code_length += 6 * sizeof(unsigned int);

    if (flags->verbose >= 2) fprintf(stderr, "Generated code will be %u bytes, %u instructions\n", code_length, code_length / sizeof(unsigned int));
    if (flags->verbose) fprintf(stderr, "Allocating memory for the output code\n");
    /* Allocate code memory which is writable and executable */
    code_memory = (unsigned int *) mmap(NULL, code_length, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (code_memory == MAP_FAILED) {
        fprintf(stderr, "Unable to map JIT code memory\n");
        perror("mmap()");

        /* Clean up used memory */
        fclose(input_file);

        return 1;
    }
    code_memory_pointer = code_memory;

    if (flags->verbose) fprintf(stderr, "Allocating memory for the runtime data\n");
    /* Allocate memory for the code to use during execution */
    jit_memory = (unsigned char *) calloc(flags->jit_memory_size, sizeof(unsigned char));
    if (jit_memory == NULL) {
        fprintf(stderr, "Unable to map JIT data memory\n");

        /* Clean up used memory */
        fclose(input_file);
        munmap(code_memory, code_length);

        return 1;
    }

    if (flags->verbose) fprintf(stderr, "Compiling code into machine code\n");
    /* Add the preamble code to the memory */
    *code_memory_pointer++ = (unsigned int) jit_memory;
    *code_memory_pointer++ = 0xe92d4080; /* push {r7, lr} */
    *code_memory_pointer++ = 0xe51f1010; /* ldr r1, [pc, #-16] */
    *code_memory_pointer++ = 0xe5d10000; /* ldrb r0, [r1] */

    /* Setup the codegen params */
    codegen_param.code_memory_pointer = &code_memory_pointer;
    codegen_param.loop_stack = malloc(flags->loop_stack_size * sizeof(unsigned int *));
    codegen_param.loop_size = 0;
    codegen_param.loop_max_size = flags->loop_stack_size;

    if (codegen_param.loop_stack == NULL) {
        fprintf(stderr, "Unable to allocate loop stack\n");

        /* Clean up used memory */
        fclose(input_file);
        munmap(code_memory, code_length);
        free(jit_memory);

        return 1;
    }

    /* Run code generation on the input file */
    rewind(input_file);
    rle_return_value = rle_read_file(input_file, rle_code_generate, &codegen_param);
    fclose(input_file);

    /* Free the loop stack from codegen params */
    free(codegen_param.loop_stack);

    /* Check if the code generation failed */
    if (rle_return_value != 0) {
        /* Clean up used memory */
        munmap(code_memory, code_length);
        free(jit_memory);

        return rle_return_value;
    }

    /* Check the loop stack size to make sure the code has no missing loop ends */
    if (codegen_param.loop_size != 0) {
        fprintf(stderr, "Input code contains loops with missing ends\n");

        /* Clean up used memory */
        munmap(code_memory, code_length);
        free(jit_memory);

        return 1;
    }

    /* Add the postamble code to the memory */
    *code_memory_pointer++ = 0xe8bd4080; /* pop {r7, lr} */
    *code_memory_pointer++ = 0xe12fff1e; /* bx lr */

    if (flags->verbose >= 2) fprintf(stderr, "Code pointer offset from start is %u bytes\n", (size_t) code_memory_pointer - (size_t) code_memory);
    if (flags->verbose) fprintf(stderr, "Running the generated code!\n\n");
    /* Call the JIT generated code */
    jit_function = (jit_function_t) (size_t) (code_memory + 1);
    jit_function();

    /* Deallocate the code and data memory */
    munmap(code_memory, code_length);
    free(jit_memory);
    return 0;
}

void parse_arguments(int argc, char *argv[], runtime_flags_t *flags) {
    /* Option character */
    unsigned char c;
    /* Temp variables for parsing numbers */
    long value;
    char *endptr;

    /* Disable default error messages */
    opterr = 0;

    /* Parse all command line input flags */
    while ((c = getopt(argc, argv, "hVvm:l:")) != 255) {
        switch (c) {
            case 'h':
                /* Output a help message */
                fprintf(stderr, "Usage: %s [options...] infile\n"
                       "    -h        show this message\n"
                       "    -V        show the version\n"
                       "    -v        enable verbose printing\n"
                       "    -m VALUE  set size of runtime memory\n"
                       "    -l VALUE  set size of loop stack during compile\n", argv[0]);
                exit(0);
            case 'V':
                /* Output the software version */
                fprintf(stderr, "BrainfuckJIT-ARM by Michiel Visser\n");
                exit(0);
            case 'v':
                /* Set verbose mode */
                flags->verbose += 1;
                break;
            case 'm':
                /* Set the runtime memory size */
                value = strtol(optarg, &endptr, 0);
                if (*endptr == '\0') {
                    if (value > 0) {
                        flags->jit_memory_size = value;
                    } else {
                        fprintf(stderr, "Runtime memory size cannot be zero or negative: %ld\n", value);
                        exit(1);
                    }
                } else {
                    fprintf(stderr, "Runtime memory size is not a number: %s\n", optarg);
                    exit(1);
                }
                break;
            case 'l':
                /* Set the loop stack size */
                value = strtol(optarg, &endptr, 0);
                if (*endptr == '\0') {
                    if (value > 0) {
                        flags->loop_stack_size = value;
                    } else {
                        fprintf(stderr, "Loop stack size cannot be zero or negative: %ld\n", value);
                        exit(1);
                    }
                } else {
                    fprintf(stderr, "Loop stack size is not a number: %s\n", optarg);
                    exit(1);
                }
                break;
            case '?':
                /* Unknown option, or missing parameter */
                if (optopt == 'm' || optopt == 'l') {
                    fprintf(stderr, "Option '-%c' requires parameter\nFor information about usage use \"%s -h\"\n", optopt, argv[0]);
                } else {
                    fprintf(stderr, "Unknown option '-%c'\nFor information about usage use \"%s -h\"\n", optopt, argv[0]);
                }
                exit(1);
        }
    }

    /* Check there is only one non-flag input */
    if (argc - optind != 1) {
        fprintf(stderr, "Expecting a single input file\nFor information about usage use \"%s -h\"\n", argv[0]);
        exit(1);
    } else {
        flags->input_file_string = argv[optind];
    }
}

int main(int argc, char *argv[]) {
    runtime_flags_t flags;

    /* Setup the default flag settings */
    flags.verbose = 0;
    flags.jit_memory_size = JIT_MEMORY_SIZE;
    flags.loop_stack_size = LOOP_STACK_SIZE;
    flags.input_file_string = NULL;

    /* Parse the arguments and run the program */
    parse_arguments(argc, argv, &flags);
    return run_jit(&flags);
}
