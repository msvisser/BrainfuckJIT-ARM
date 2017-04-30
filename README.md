# BrainfuckJIT-ARM
This is a Just in Time compiler for Brainfuck that runs on the ARMv7 platform.
It will do a very simple run length encoding to optimise the program and squash repeat operations into a single operation.

## Usage
You can simply run the JIT compiler with an input file as arguments like `bfjit input.bf`. In the case you want more verbose information from the JIT compiler next to your program output you can run `bfjit -v input.bf`. 
Furthermore you have to option to change the runtime memory size with the `-m` flag, as follows `bfjit -m 30000 input.bf`. Where the parameter is the number of cells.
Finally you can change the compilers maximum loop depth with the `-l` flag, as follows `bfjit -l 100 input.bf`. With the default value being 100 it will be very unlikely that you will actually need to change this, but if your program has a lot of nested loops you might need to increase it. You can also find this information by running `bfjit -h`.
