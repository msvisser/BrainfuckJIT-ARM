# BrainfuckJIT-ARM
This is a Just in Time compiler for Brainfuck that runs on the ARMv7 platform.
It will do a very simple run length encoding to optimise the program and squash repeat operations into a single operation.

## Usage
You can simply run the JIT compiler with an input file as arguments like `bfjit input.bf`. In the case you want more verbose information from the JIT compiler next to your program output you can run `bfjit -v input.bf`. 
Furthermore you have to option to change the runtime memory size with the `-m` flag, as follows `bfjit -m 30000 input.bf`. Where the parameter is the number of cells.
Finally you can change the compilers maximum loop depth with the `-l` flag, as follows `bfjit -l 100 input.bf`. With the default value being 100 it will be very unlikely that you will actually need to change this, but if your program has a lot of nested loops you might need to increase it. You can also find this information by running `bfjit -h`.

## How does it work
THe JIT compiler will run a very simple RLE scheme on the input data just combining multiple repeating characters. The characters `+`, `-`, `>` and `<` can be executed multiple times in one instruction. While `[`, `]`, `.` and `,` just have to be repeated multiple times. Below the equivalent ARM assembly is shown for the generated program.

### Preamble
First of all the beginning of the function is set up. Here we push some registers on the stack and load the address of the memory block into `r4`. Finally we load `r0` with the value of the cell at `r4`, `r0` is used as a temporary register, which will hold the value of the current cell.
```asm
    .word cells_memory_address
main:
    push {r4, r7, fp, lr}
    mov fp, sp
    ldr r4, [pc, #-20]
    ldrb r0, [r4]
```

### Adding and Subtracting
Adding and subtracting simply operate on `r0` as it contains the value of the current cell. Multiple consecutive instances of the operation can be combined into a single instruction.
```asm
    add r0, r0, #count
    sub r0, r0, #count
```

### Moving left and right
Moving around requires the value in `r0` to be stored in memory, as we are changing the current cell. After that we will need to move the pointer in `r4` based on the number of move instructions. Finally we need to load the new value into `r0`. We can optimise the store and move instruction into one, as ARM allows postincrementing. In this case count can be either positive or negative, based on the direction of movement.
```asm
    strb r0, [r4], #count
    ldrb r0, [r4]
```

### Loops
Loops are a little more complex as they require keeping track of the offsets. In this compiler the loop start code will be generated with a blank offset. The compiler will save the memory location of this branch instrucion on the jump stack. When a loop end if generated it will look at the loop stack to figure out what the offsets should be and it will generate the code for the end of the loop. It will also modify the loop start code to include the correct offset. Instead of comparing to 0 we will test against 255, which will make sure we only consider the lowest eight bits. 
```asm
    tst r0, #255
    beq .Lloopend
.Lloopbegin:
    
    tst r0, #255
    bne .Lloopbegin
.Lloopend
```

### Input and Output
Input and output is handled by running systemcalls for read and write. Before we can call such a systemcall the value in `r0` has to be stored in memory, otherwise the systemcall cannot access it. After that the systemcall is executed. Finally we have to load the value for `r0` again from memory, as the systemcall trashes the value in `r0`.

__Input:__
```asm
    strb r0, [r4]
    mov r7. #3
    mov r0, #0
    mov r1, r4
    mov r2, #1
    svc #0
    ldrb r0, [r4]
```
__Output:__
```asm
    strb r0, [r4]
    mov r7. #4
    mov r0, #1
    mov r1, r4
    mov r2, #1
    svc #0
    ldrb r0, [r4]
```

### Postamble
Finally the program has to return control back to the JIT compiler. Therefore we need a postamble, which corrects `sp` and pops the registers that were pushed in the preamble.
```asm
    mov sp, fp
    pop {r4, r7, fp, lr}
    bx lr
```
