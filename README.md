# HangmanServer
Console-based server for multiplayer hangman.

Usage:
suppose the port you want to communicate in is 12345
To compile: $make PORT=12345
To initialize server: $./server dictionary.txt
To connect (on a different shell): nc -C [-c on MacOS] localhost 12345

Supports multiple clients and dynamic entering/exit of clients.
