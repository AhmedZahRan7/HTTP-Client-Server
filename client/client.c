#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <poll.h>
#include <ctype.h>

#define MAX_LINE_SIZE 1000
#define MAX_DATA_SIZE 100000
#define CONNECTION_TIMEOUT 5
// void* to return a pointer [like T]
void* castToRightSocketAddress(struct sockaddr *address){
    if (address->sa_family == AF_INET) return &(((struct sockaddr_in*)address)->sin_addr);
    return &(((struct sockaddr_in6*)address)->sin6_addr);
}

struct addrinfo* getServerInfo(char* ip,char* port){
    struct addrinfo socketInformation;
    memset(&socketInformation, 0, sizeof socketInformation);
    socketInformation.ai_family = AF_UNSPEC; //ipv4 or ipv6
    socketInformation.ai_socktype = SOCK_STREAM; //TCP
    socketInformation.ai_flags = AI_PASSIVE; // use my IP
    
    struct addrinfo *result;
    if (getaddrinfo(ip, port, &socketInformation, &result) != 0) {
        printf("can't get server information\n");
        exit(1);
    }
     // result is a node in a linkedlist
    return result;
}

int getValidDescriptor(struct addrinfo *results){
    int socketDescriptor;
    // loop through the linkedlist and get the first available descriptor
    while(
        results != NULL &&
        (socketDescriptor = 
            socket(
                results->ai_family,
                results->ai_socktype,
                results->ai_protocol
            )) == -1) results = results->ai_next;
    
    return socketDescriptor;
}

int connectToServer(char* ip,char* port){
    struct addrinfo *info = getServerInfo(ip,port);
    int socketDescriptor = getValidDescriptor(info);
    while(connect(socketDescriptor, info->ai_addr, info->ai_addrlen) == -1){
        close(socketDescriptor);
        socketDescriptor = getValidDescriptor(info);
    }
    if(info == NULL) {
        printf("can't connect to the server\n");
    }
    
    char s[INET6_ADDRSTRLEN];
    inet_ntop(info->ai_family, castToRightSocketAddress((struct sockaddr *)info->ai_addr),s, sizeof s);
    printf("connected to %s\n", s);
    freeaddrinfo(info);
    return socketDescriptor;
}

void sendToServer(int connection, char* msg){
    if(send(connection,msg,strlen(msg),0) == -1) printf("can't send data");
}


void receieveResponse(int connection){
    //list of sockets to monitor events [only one socket in our case] 
    struct pollfd socketMonitor[1];
    socketMonitor[0].fd = connection;
    socketMonitor[0].events = POLL_IN;
    while(1){
        // poll if the socket had new event to handle or not.
        int numOfEvents = poll(socketMonitor,1, CONNECTION_TIMEOUT*1000);
        if(numOfEvents == 0) break; // no more IN events happend during the timeout interval
        char *buffer = (char*)malloc(sizeof(char) * MAX_DATA_SIZE);
        int receivedBytes = recv(connection,buffer,MAX_DATA_SIZE-1,0);
        if(receivedBytes == 0) break; // the client closed the connection
        if(receivedBytes == -1){
            printf("Error when receiving from the client\n");
            exit(1);
        }
        printf("%s",buffer);
        free(buffer);
    }
}

int isEmptyLine(char *line){
    for(int i=0;i<strlen(line);i++) if(!isspace(line[i])) return 0;
    return 1;
}

int main(int argc, char **argv){
    if(argc != 3){
        printf("Invalid num of arguments\n");
        exit(1);
    }
    char *SERVER_IP = argv[1];
    char *SERVER_PORT = argv[2];
    int connection = connectToServer(SERVER_IP,SERVER_PORT);

    int emptyLines = 0;
    while(1){
        char *line = (char*)malloc(sizeof(char) * MAX_LINE_SIZE);
        size_t size = MAX_LINE_SIZE;
        if (getline(&line, &size, stdin) == -1 || strcmp(line,"close\n") == 0) break;
        sendToServer(connection,line);
        emptyLines+=isEmptyLine(line);
        if(emptyLines >= 2) {
            receieveResponse(connection);
            emptyLines = 0;
        }
        free(line);
    }
    close(connection);
    return 0;
}
