#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>

/* Buffer size settings, these influence the runtime memory footprint */
#define INPUT_BUFFER_SIZE 512
#define LOOP_STACK_SIZE 100
#define JIT_MEMORY_SIZE 30000

/* Function types for the RLE callback and the actual JIT function */
typedef void (*rle_callback_t)(unsigned char, unsigned int, void *);
typedef void (*jit_function_t)(void);

/* Structure for passing the code generation data to the RLE function */
typedef struct {
	unsigned int **code_memory_pointer;
	unsigned int *loop_stack[LOOP_STACK_SIZE];
	unsigned int loop_size;
} codegen_param_t;

/* Read all characters from a file and call the handle_char callback for every
   sequence of equal characters, effectively RLEing the input file */
void rle_read_file(FILE *file, rle_callback_t handle_char, void *param) {
	unsigned char *input_buffer;
	unsigned int read_length;
	unsigned char last_character, this_character;
	unsigned int last_count;
	unsigned int i;

	/* Allocate a buffer for the input */
	input_buffer = malloc(INPUT_BUFFER_SIZE);

	/* Set up the RLE variables */
	last_character = '\0';
	last_count = 0;

	do {
		/* Read a max of INPUT_BUFFER_SIZE from the input file */
		read_length = fread(input_buffer, sizeof(unsigned char), INPUT_BUFFER_SIZE, file);
		/* Iterate over all characters that were read */
		for (i = 0; i < read_length; i++) {
			this_character = *(input_buffer + i);
			if (this_character != last_character) {
				handle_char(last_character, last_count, param);
				last_character = this_character;
				last_count = 1;
			} else {
				last_count++;
			}
		}
	} while (read_length);
	/* Finish up final characters */
	if (last_count > 0) {
		handle_char(last_character, last_count, param);
	}

	/* Deallocate the input buffer */
	free(input_buffer);
}

/* Callback function for rle_read_file which will calculate the total number of instructions
   this program will take once compiled */
void rle_determine_code_length(unsigned char character, unsigned int count, void *param) {
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
			*code_length += 7 * count * sizeof(unsigned int);
			break;
	}
}

/* Callback function for rle_read_file which will actually do the code generation on the file, 
   this function requires an instance of codegen_param_t as input parameter */
void rle_code_generate(unsigned char character, unsigned int count, void *param) {
	codegen_param_t *codegen_param = (codegen_param_t *) param;
	unsigned int **code_memory_pointer = codegen_param->code_memory_pointer;
	unsigned int *code_memory = *code_memory_pointer;
	unsigned int i;

	/* Add number of bytes to the parameter base on character and count */
	switch (character) {
		/* "+-><" can be repeated in the same instruction */
		case '+':
			if (count > 0xff) exit(2);
			*(code_memory + 0) = 0xe2800000 | (count & 0xff); /* add r0, r0, count */
			*code_memory_pointer += 1;
			break;
		case '-':
			if (count > 0xff) exit(2);
			*(code_memory + 0) = 0xe2400000 | (count & 0xff); /* sub r0, r0, count */
			*code_memory_pointer += 1;
			break;
		case '>':
			if (count > 0xfff) exit(2);
			*(code_memory + 0) = 0xe4c40000 | (count & 0xfff); /* str r0, [r4], count */
		 	*(code_memory + 1) = 0xe5d40000; /* ldr r0, [r4] */
			*code_memory_pointer += 2;
			break;
		case '<':
			if (count > 0xfff) exit(2);
			*(code_memory + 0) = 0xe4440000 | (count & 0xfff); /* str r0, [r4], -count */
		 	*(code_memory + 1) = 0xe5d40000; /* ldr r0, [r4] */
			*code_memory_pointer += 2;
			break;
		/* "[].," need to be repeated for every character */
		case '[':
			for (i = 0; i < count; i++) {
				*(code_memory + (i * 2) + 0) = 0xe31000ff; /* tst r0, #255 */
				*(code_memory + (i * 2) + 1) = 0x0a000000; /* beq offset */

				/* Save the memory address of the branch instruction on the stack, so we
				   can fix the offset when we find the closing bracket */
				if (codegen_param->loop_size >= LOOP_STACK_SIZE) exit(3);
				codegen_param->loop_stack[codegen_param->loop_size++] = (code_memory + (i * 2) + 1);
			}
			*code_memory_pointer += 2 * count;
			break;
		case ']':
			for (i = 0; i < count; i++) {
				unsigned int *back_addr, *cur_addr, back_offset, forward_offset;
				/* Get the location of the opening bracket for this closing bracket */
				if (codegen_param->loop_size == 0) exit(3);
				back_addr = codegen_param->loop_stack[--codegen_param->loop_size];
				cur_addr = (code_memory + (i * 2) + 1);

				/* Determine the branching offsets for the two branch instructions of this bracket pair */
				back_offset = ((unsigned int) back_addr - (unsigned int) cur_addr - 4) >> 2;
				forward_offset = ((unsigned int) cur_addr - (unsigned int) back_addr - 4) >> 2;

				*(code_memory + (i * 2) + 0) = 0xe31000ff; /* tst r0, #255 */
				*(code_memory + (i * 2) + 1) = 0x1a000000 | (back_offset & 0xffffff); /* bne offset */
				*((unsigned int *) back_addr) |= (forward_offset & 0xffffff);
			}
			*code_memory_pointer += 2 * count;
			break;
		case '.':
			for (i = 0; i < count; i++) {
				/* Run a systemcall write(stdout, jit_memory[current_cell], 1) */
				*(code_memory + (i * 7) + 0) = 0xe5c40000; /* str r0, [r4] */			
				*(code_memory + (i * 7) + 1) = 0xe3a07004; /* mov r7, #4 */
				*(code_memory + (i * 7) + 2) = 0xe3a00001; /* mov r0, #1 */
				*(code_memory + (i * 7) + 3) = 0xe1a01004; /* mov r1, r4 */
				*(code_memory + (i * 7) + 4) = 0xe3a02001; /* mov r2, #1 */
				*(code_memory + (i * 7) + 5) = 0xef000000; /* svc #0 */
				*(code_memory + (i * 7) + 6) = 0xe5d40000; /* ldr r0, [r4] */
			}
			*code_memory_pointer += 7 * count;
			break;
		case ',':
			for (i = 0; i < count; i++) {
				/* Run a systemcall read(stdin, jit_memory[current_cell], 1) */
				*(code_memory + (i * 7) + 0) = 0xe5c40000; /* str r0, [r4] */
				*(code_memory + (i * 7) + 1) = 0xe3a07003; /* mov r7, #3 */
				*(code_memory + (i * 7) + 2) = 0xe3a00000; /* mov r0, #0 */
				*(code_memory + (i * 7) + 3) = 0xe1a01004; /* mov r1, r4 */
				*(code_memory + (i * 7) + 4) = 0xe3a02001; /* mov r2, #1 */
				*(code_memory + (i * 7) + 5) = 0xef000000; /* svc #0 */
				*(code_memory + (i * 7) + 6) = 0xe5d40000; /* ldr r0, [r4] */		
			}
			*code_memory_pointer += 7 * count;
			break;
	}
}

int main(int argc, char *argv[]) {
	/* Input variables */
	FILE *input_file;
	/* JIT variables */
	unsigned int code_length;
	unsigned int *code_memory;
	unsigned int *code_memory_pointer;
	codegen_param_t codegen_param;
	unsigned char *jit_memory;
	jit_function_t jit_function;
	
	/* Open the input file */
	input_file = fopen("input.bf", "r");
	if (input_file == NULL) {
		printf("Could not open file\n");
		return 1;
	}

	/* Reset code length */
	code_length = 0;

	printf("Determining the output code length\n");
	/* Read the input and determine the code size */
	rle_read_file(input_file, rle_determine_code_length, &code_length);

	/* Increase the code length to allow for pre- and postamble */
	code_length += 9 * sizeof(unsigned int);

	printf("Allocating memory for the output code\n");
	/* Allocate code memory which is writable and executable */		
	code_memory = (unsigned int *) mmap(NULL, code_length, PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (code_memory == MAP_FAILED) {
		printf("Unable to map JIT code memory\n");
		perror("mmap()");
		return 1;
	}
	code_memory_pointer = code_memory;

	printf("Allocating memory for the runtime data\n");
	/* Allocate memory for the code to use during execution */
	jit_memory = (unsigned char *) malloc(JIT_MEMORY_SIZE);
	if (jit_memory == NULL) {
		printf("Unable to map JIT data memory\n");
		return 1;
	}
	memset(jit_memory, 0, JIT_MEMORY_SIZE);

	printf("Compiling code into machine code\n");
	/* Add the preamble code to the memory */
    *code_memory_pointer++ = (unsigned int) jit_memory;
    *code_memory_pointer++ = 0xe92d4890; /* push {r4, r7, fp, lr} */
    *code_memory_pointer++ = 0xe1a0b00d; /* mov fp, sp */
    *code_memory_pointer++ = 0xe51f4014; /* ldr r4, [pc, #-20] */
    *code_memory_pointer++ = 0xe5d40000; /* ldrb r0, [r4] */

    /* Setup the codegen params */
    codegen_param.code_memory_pointer = &code_memory_pointer;
    codegen_param.loop_size = 0;

	/* Run code generation on the input file */
	rewind(input_file);
	rle_read_file(input_file, rle_code_generate, &codegen_param);

	/* Add the postamble code to the memory */	
    *code_memory_pointer++ = 0xe1a0d00b; /* mov sp, fp */
    *code_memory_pointer++ = 0xe8bd4890; /* pop {r4, r7, fp, lr} */
    *code_memory_pointer++ = 0xe3a00000; /* mov r0, #0 */
    *code_memory_pointer++ = 0xe12fff1e; /* bx lr */

	printf("Running the generated code!\n\n");
    /* Call the JIT generated code */
    jit_function = (jit_function_t) (code_memory + 1);
    jit_function();

	/* Deallocate the code and data memory */
	munmap(code_memory, code_length);
	free(jit_memory);
	return 0;
}