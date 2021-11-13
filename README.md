# Socket Programming in C
### Part1 :Multi-threaded Web Server
##### what is web server?
“A web server is computer software and underlying hardware that accepts requests via HTTP, the network protocol created to distribute web content.” The web server should accept incoming connection requests. It should then look for the GET request and pick out the name of the requested file. If the request is POST then it sends an OK message and waits for the uploaded file from the client. the server should keep the connection open waiting for new requests from the same client. HTTP/1.1 support should be added to the server, consisting of persistent connections and pipelining of client requests to the web browser, also handling the dynamic timeout for the connections according to the number of the active connections.

##### Psedudo code

```
//main program
create socket with the given port number
bind this socket
start listening for the incoming requests
infinite loop :
    if the number of current connections is the maximum -> wait on semaphore
    accept one of pending connections
    create new thread to handle this connection
    
//threads work
increase the number of active connections
re-assign timeout interval
infinite loop :
    for the timeout interval : poll on the socket to check incoming data
    if no data on the socket during this interval [timeout] -> break
    start recv()ing data and store in a buffer
start parsing the data in buffer
close this connection
decrease the number of active connections

//parsing data
if method = GET:
    if the requested file not exist -> send "NOT FOUND"
    else -> send "OK" and start streaming the file to the client
else if method = POST:
    send "OK"
    decapsulate the headers
    save the file in the base directory
else :
    send "NOT SUPPORTED METHOD"
```
##### Assumptions
As the headers of the request are not parsed, so there is no way to get the content length, so assume that the content ends with “\r\n\r\n”

##### User guide
```bash
To compile and run the server on a port_num
$ cd server directory
$ gcc -pthread -o server server.c
$ ./server port_num
```

### Part2 :HTTP Web Client
##### Introduction
The web client must read and parse a series of commands from the input file. Only the GET and POST commands are handled. The commands syntax should be as follows, where file path is the path of the file on the server:
```
get file-path host-name port-number
post file-path host-name port-number
```
The default HTTP port number is 80. In response to the specified operation (GET or POST), the client must open a connection to an HTTP server on the specified host listening on the specified (or default) port number. The receiver must display the file and then store it in the local directory (i.e., the directory from which the client or server program was run). The client should shut down when reaching the end of the file.

##### Pseudo Code
```
while input file contains more lines:
    parse the current line
    connect to the server with (ip,port)
    if the method is "user_get" :
        send "GET" + filePath + protocol to server
        receive response from server
        create a file with the same path and write the data received from the server
    else if the method is "user_post":
        send "POST" + filePath + protocol to server
        read the data from the specified file
        send data to the server
        receive the response
    else :
        "Not supported function"
    close the connection
```
##### User guide
```bash
To compile and run the client with set of requests
$ cd client directory
$ gcc -pthread -o client client.c
$ ./client input/file/path
```
