#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "socket.h"

#define BUFFER_LENGTH 4096
#define MAXIMUM_NUMBER_LENGTH 33

#define WRITE_STRING_OR_EXIT(dataName, dataString) \
if (writeSocket(socket, dataString, strlen(dataString) + 1) != 0) { \
  printSocketError("ERROR: Couldn't send " dataName); \
  return 1; \
}

// This is a list of valid environment variables that GitHub Desktop might
// send or expect to receive.
#define NUMBER_OF_VALID_ENV_VARS 4
static const char *sValidEnvVars[NUMBER_OF_VALID_ENV_VARS] = {
  "DESKTOP_TRAMPOLINE_IDENTIFIER",
  "DESKTOP_TRAMPOLINE_TOKEN",
  "DESKTOP_USERNAME",
  "DESKTOP_ENDPOINT",
};

/** Returns 1 if a given env variable is valid, 0 otherwise. */
int isValidEnvVar(char *env) {
  for (int idx = 0; idx < NUMBER_OF_VALID_ENV_VARS; idx++) {
    const char *candidate = sValidEnvVars[idx];

    // Make sure that not only the passed env var string starts with the
    // candidate contesnts, but also that there is a '=' character right after:
    // Valid: "DESKTOP_USERNAME=sergiou87"
    // Not valid: "DESKTOP_USERNAME_SOMETHING=sergiou87"
    if (strncmp(env, candidate, strlen(candidate)) == 0
        && strlen(env) > strlen(candidate)
        && env[strlen(candidate)] == '=') {
      return 1;
    }
  }

  return 0;
}

/**
 * Reads a string from the socket, reading first 2 bytes to get its length and
 * then the string itself.
 */
ssize_t readDelimitedString(SOCKET socket, char *buffer, size_t bufferSize) {
  uint16_t outputLength = 0;
  if (readSocket(socket, &outputLength, sizeof(uint16_t)) < (int)sizeof(uint16_t)) {
      printSocketError("ERROR: Error reading from socket");
      return -1;
  }

  if (outputLength > bufferSize) {
    fprintf(stderr, "ERROR: received string is bigger than buffer (%d > %zu)", outputLength, bufferSize);
    return -1;
  }

  size_t totalBytesRead = 0;
  ssize_t bytesRead = 0;

  // Read output from server
  do {
    bytesRead = readSocket(socket, buffer + totalBytesRead, outputLength - totalBytesRead);

    if (bytesRead == -1) {
      printSocketError("ERROR: Error reading from socket");
      return -1;
    }

    totalBytesRead += bytesRead;
  } while (bytesRead > 0);

  buffer[totalBytesRead] = '\0';

  return totalBytesRead;
}

int runTrampolineClient(SOCKET *outSocket, int argc, char **argv, char **envp) {
  char *desktopPortString;

  desktopPortString = getenv("DESKTOP_PORT");

  if (desktopPortString == NULL) {
    fprintf(stderr, "ERROR: Missing DESKTOP_PORT environment variable\n");
    return 1;
  }

  unsigned short desktopPort = atoi(desktopPortString);

  SOCKET socket = openSocket();

  if (socket == INVALID_SOCKET) {
    printSocketError("ERROR: Couldn't create TCP socket");
    return 1;
  }

  *outSocket = socket;

  if (connectSocket(socket, desktopPort) != 0) {
    printSocketError("ERROR: Couldn't connect to 127.0.0.1:%d", desktopPort);
    return 1;
  }

  // Send the number of arguments (except the program name)
  char argcString[MAXIMUM_NUMBER_LENGTH];
  snprintf(argcString, MAXIMUM_NUMBER_LENGTH, "%d", argc - 1);
  WRITE_STRING_OR_EXIT("number of arguments", argcString);

  // Send each argument separated by \0
  for (int idx = 1; idx < argc; idx++) {
    WRITE_STRING_OR_EXIT("argument", argv[idx]);
  }

  // Get the number of environment variables
  char *validEnvVars[NUMBER_OF_VALID_ENV_VARS];
  int envc = 0;
  for (char **env = envp; *env != 0; env++) {
    if (isValidEnvVar(*env)) {
      validEnvVars[envc] = *env;
      envc++;
    }
  }

  // Send the number of environment variables
  char envcString[MAXIMUM_NUMBER_LENGTH];
  snprintf(envcString, MAXIMUM_NUMBER_LENGTH, "%d", envc);
  WRITE_STRING_OR_EXIT("number of environment variables", envcString);

  // Send the environment variables
  for (int idx = 0; idx < envc; idx++) {
    WRITE_STRING_OR_EXIT("environment variable", validEnvVars[idx]);
  }

  char buffer[BUFFER_LENGTH + 1];
  size_t totalBytesWritten = 0;
  ssize_t bytesToWrite = 0;

  // Make stdin reading non-blocking, to prevent getting stuck when no data is
  // provided via stdin.
  int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

  // Send stdin data
  do {
    bytesToWrite = read(0, buffer, BUFFER_LENGTH);

    if (bytesToWrite == -1) {
      if (totalBytesWritten == 0) {
        // No stdin content found, continuing...
        break;
      } else {
        fprintf(stderr, "ERROR: Error reading stdin data");
        return 1;
      }
    }

    if (writeSocket(socket, buffer, bytesToWrite) != 0) {
      printSocketError("ERROR: Couldn't send stdin data");
      return 1;
    }

    totalBytesWritten += bytesToWrite;
  } while (bytesToWrite > 0);

  writeSocket(socket, "\0", 1);

  // Read stdout from the server
  if (readDelimitedString(socket, buffer, BUFFER_LENGTH) == -1) {
    fprintf(stderr, "ERROR: Couldn't read stdout from socket");
    return 1;
  }

  // Write that output to stdout
  fprintf(stdout, "%s", buffer);

  // Read stderr from the server
  if (readDelimitedString(socket, buffer, BUFFER_LENGTH) == -1) {
    fprintf(stderr, "ERROR: Couldn't read stdout from socket");
    return 1;
  }

  // Write that output to stderr
  fprintf(stderr, "%s", buffer);

  return 0;
}

int main(int argc, char **argv, char **envp) {
  if (initializeNetwork() != 0) {
    return 1;
  }

  SOCKET socket = INVALID_SOCKET;
  int result = runTrampolineClient(&socket, argc, argv, envp);  

  if (socket != INVALID_SOCKET)
  {
    closeSocket(socket);
  }

  terminateNetwork();

  return result;
}
