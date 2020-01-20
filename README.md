# HangmanServer
Console-based server for multiplayer hangman.
Supports multiple clients and dynamic entering/exit of clients.


#### Usage:

Suppose the port you want to communicate in is 12345

To compile: $make PORT=

To initialize server: $./server dictionary.txt

To connect (on a different terminal): nc -C [-c on MacOS] localhost 12345
