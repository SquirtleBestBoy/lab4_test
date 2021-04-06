#include <fnmatch.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#define BACKLOG (10)

static char const response_str[] = "HTTP/1.0 200 OK\r\n"
        "Content-type: text/html; charset=UTF-8\r\n\r\n";

/* char* parseRequest(char* request)
 * Args: HTTP request of the form "GET /path/to/resource HTTP/1.X"
 *
 * Return: the resource requested "/path/to/resource"
 *         0 if the request is not a valid HTTP request
 *
 * Does not modify the given request string.
 * The returned resource should be free'd by the caller function.
 */
static char *parseRequest(char *request) {
  //assume file paths are no more than 4095 bytes + 1 for null.
  char *buffer = calloc(4096, 1);

  int version = 0;
  char cr = 0;
  char nl = 0;
  if (sscanf(request, "GET %4096s HTTP/1.%d%c%c", buffer, &version, &cr, &nl) != 4
      || cr != '\r' || nl != '\n')
  {
    free(buffer);
    return 0;
  }

  return buffer;
}

static void send_error(int client_fd, int http_status_code) {
  char const *status = "";
  switch (http_status_code) {
  case 400:
    status = "Bad Request";
    break;
  }

  char response[256];
  snprintf(response, sizeof response, "HTTP/1.0 %d %s\r\nConnection: close\r\n\r\n",
           http_status_code, status);
  send(client_fd, response, strlen(response), 0);
}

static void serve_request(int client_fd){
  size_t offset = 0;
  char buffer[4096] = { 0 };

  while (offset < sizeof buffer) {
    ssize_t received = recv(client_fd, &buffer[offset],
                            sizeof buffer - offset, 0);
    if (strstr(buffer + offset,"\r\n\r\n"))
      break;
    offset += received;
  }

  char *requested_file = parseRequest(buffer);
  if (!requested_file) {
    send_error(client_fd, 400); // Bad Request
    return;
  }

  /* Make sure the requested_file starts with a / and does not contain /../
   * anywhere.
   */
  if (requested_file[0] != '/' || strstr(requested_file, "/../")) {
    send_error(client_fd, 400); // Bad Request
    free(requested_file);
    return;
  }

  send(client_fd, response_str, sizeof(response_str) - 1, 0);

  // take requested_file, add a . to beginning, open that file
  char *file_path = malloc(strlen(requested_file) + 2);
  file_path[0] = '.';
  strcpy(file_path + 1, requested_file);
  free(requested_file);

  int read_fd = open(file_path, O_RDONLY);
  free(file_path);

  while(1){
    ssize_t bytes_read = read(read_fd, buffer, sizeof buffer);
    send(client_fd, buffer, bytes_read, 0);
  }

  close(read_fd);
}

/* Your program should take two arguments:
 * 1) The port number on which to bind and listen for connections, and
 * 2) The directory out of which to serve files.
 */
int main(int argc, char **argv) {
    /* For checking return values. */
    int retval;

    /* Read the port number from the first command line argument. */
    int port = atoi(argv[1]);

    /* Create a socket to which clients will connect. */
    int server_sock = socket(AF_INET6, SOCK_STREAM, 0);
    if(server_sock < 0) {
        perror("Creating socket failed");
        exit(1);
    }

    /* A server socket is bound to a port, which it will listen on for incoming
     * connections.  By default, when a bound socket is closed, the OS waits a
     * couple of minutes before allowing the port to be re-used.  This is
     * inconvenient when you're developing an application, since it means that
     * you have to wait a minute or two after you run to try things again, so
     * we can disable the wait time by setting a socket option called
     * SO_REUSEADDR, which tells the OS that we want to be able to immediately
     * re-bind to that same port. See the socket(7) man page ("man 7 socket")
     * and setsockopt(2) pages for more details about socket options. */
    int opt = 1;
    retval = setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    if (retval < 0) {
        perror("Setting socket option SO_REUSEADDR failed");
        exit(1);
    }

    /* Allow IPv4 to connect as well. */
    opt = 0;
    retval = setsockopt(server_sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof opt);
    if (retval < 0) {
      perror("Setting socket option IPV6_V6ONLY failed");
      exit(1);
    }

    /* Create an address structure.  This is very similar to what we saw on the
     * client side, only this time, we're not telling the OS where to connect,
     * we're telling it to bind to a particular address and port to receive
     * incoming connections.  Like the client side, we must use htons() to put
     * the port number in network byte order.  When specifying the IP address,
     * we use a special constant, in6addr_any, which tells the OS to bind to all
     * of the system's addresses.  If your machine has multiple network
     * interfaces, and you only wanted to accept connections from one of them,
     * you could supply the address of the interface you wanted to use here. */


    struct sockaddr_in6 addr;   // internet socket address data structure
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port); // byte order is significant
    addr.sin6_addr = in6addr_any; // listen to all interfaces


    /* As its name implies, this system call asks the OS to bind the socket to
     * address and port specified above. */
    retval = bind(server_sock, (struct sockaddr*)&addr, sizeof(addr));
    if(retval < 0) {
        perror("Error binding to port");
        exit(1);
    }

    /* Now that we've bound to an address and port, we tell the OS that we're
     * ready to start listening for client connections.  This effectively
     * activates the server socket.  BACKLOG (#defined above) tells the OS how
     * much space to reserve for incoming connections that have not yet been
     * accepted. */
    retval = listen(server_sock, BACKLOG);
    if(retval < 0) {
        perror("Error listening for connections");
        exit(1);
    }

    while(1) {
        /* Another address structure.  This time, the system will automatically
         * fill it in, when we accept a connection, to tell us where the
         * connection came from. */
        struct sockaddr_storage remote_addr;
        socklen_t socklen = sizeof(remote_addr);

        /* Accept the first waiting connection from the server socket and
         * populate the address information.  The result (sock) is a socket
         * descriptor for the conversation with the newly connected client.  If
         * there are no pending connections in the back log, this function will
         * block indefinitely while waiting for a client connection to be made.
         * */
        int sock = accept(server_sock, (struct sockaddr*) &remote_addr, &socklen);
        if(sock < 0) {
            perror("Error accepting connection");
            exit(1);
        }

        /* At this point, you have a connected socket (named sock) that you can
         * use to send() and recv(). */

        /* ALWAYS check the return value of send().  Also, don't hardcode
         * values.  This is just an example.  Do as I say, not as I do, etc. */
        serve_request(sock);

        /* Tell the OS to clean up the resources associated with that client
         * connection, now that we're done with it. */
        close(sock);
    }

    close(server_sock);
}
