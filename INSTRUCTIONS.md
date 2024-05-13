# Implementation of `Guess a word` in C++ (by Tadeáš Souček)
## Running the test
- Use provided binaries, or compile by simply running `build.sh` script provided (`g++` must be installed on target machine)
- Read user credentials from `registeredUsers.txt` and use them to test the app (or add a custom account). This is only a test case, but of course, passwords should be properly hashed in real world applications.

## Protocol
- This implementation uses simple [msgtype, length, payload] protocol
- Payload format is different for each request type, and covers various protocol types (binary for control messages, plaintext when sending a hint to opponent, NUL separated when sending username:password))
- Currently has buffer size limit, but it's enough for the simple program (However, I'm also famillar with heap allocated memory, (malloc, realloc, free))

## Parallelism
- Server uses Linux kernel's epoll API, which provides non-blocking I/O without need of spawning new thread for each connection
- Client uses poll API, which is just older and little different version of epoll (But i have no problem with using any of these non-blocking APIs (select, poll, epoll, etc.))
- On server, every [msgtype, length, payload] message is only written into buffer and written later, when write ready (Read also uses buffering)

## Coding style
- In this example, i primarily use functional coding paradigm, but some object oriented code is also included (especially inline methods related to some structure, but I'm also famillar with virtual methods, when more modularity is needed)
- I primarily use C++, but i also use Python for scripts, that are not performance-critical
