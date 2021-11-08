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

#define MAX_WAITING_CONNECTIONS 5
#define CONNECTION_TIMEOUT 10
#define MAX_DATA_SIZE 10000
#define OK_RESPONSE "HTTP/1.1 200 OK\n\n"
#define FILE_NOT_FOUND_RESPONSE "HTTP/1.1 404 Not Found\n\n"
#define METHOD_NOT_FOUND_RESPONSE "HTTP/1.1 405 Method Not Allowed\n\n"

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

int bindTheSocket(int socketDescriptor, struct addrinfo *results){
    return bind(socketDescriptor,results->ai_addr,results->ai_addrlen);
}

struct addrinfo* getServerInfo(char* port){
    struct addrinfo socketInformation;
    memset(&socketInformation, 0, sizeof socketInformation);
    socketInformation.ai_family = AF_UNSPEC; //ipv4 or ipv6
    socketInformation.ai_socktype = SOCK_STREAM; //TCP
    socketInformation.ai_flags = AI_PASSIVE; // use my IP
    struct addrinfo *result;
    if (getaddrinfo(NULL, port, &socketInformation, &result) != 0) {
        printf("can't get server information\n");
        exit(1);
    }
    // result is a node in a linkedlist
    return result;
}

int createSocket(struct addrinfo* results){
    int descriptor = getValidDescriptor(results);
    while (bindTheSocket(descriptor,results) == -1){
        close(descriptor);
        descriptor = getValidDescriptor(results);
    }

    return descriptor;
}

void listenOnSocket(int descriptor){
    if(listen(descriptor,MAX_WAITING_CONNECTIONS) == -1){
        printf("Can't listen on socket\n");
        exit(1);
    }
}
// void bindTheSocket(){
//     if (setsockopt(socketDescriptor, SOL_SOCKET, SO_REUSEADDR, &yes,sizeof(int)) == -1) {
//         perror("setsockopt");
//         exit(1);
// }

// void* to return a pointer [like T]
void* castToRightSocketAddress(struct sockaddr *address){
    if (address->sa_family == AF_INET) return &(((struct sockaddr_in*)address)->sin_addr);
    return &(((struct sockaddr_in6*)address)->sin6_addr);
}

int acceptConnections(int socketDescriptor){
    // large enough to hold both IPv4 and IPv6 structures.
    struct sockaddr_storage clientAddress; 
    socklen_t addressSize = sizeof clientAddress;
    int connectionDescriptor = accept(socketDescriptor, (struct sockaddr *)&clientAddress, &addressSize);
    if(connectionDescriptor == -1){
        printf("Can't accept connection\n");
        exit(1);
    }

    char clientAddressPresentation[INET6_ADDRSTRLEN];
    inet_ntop(clientAddress.ss_family,castToRightSocketAddress((struct sockaddr *)&clientAddress),clientAddressPresentation, sizeof clientAddressPresentation);
    printf("connection %d with machine %s\n", connectionDescriptor ,clientAddressPresentation);
    return connectionDescriptor;
}

char* readFile(char* filePath){
    FILE * fPtr;
    char ch;
    char* content  = malloc(sizeof(char) * MAX_DATA_SIZE);
    fPtr = fopen(filePath, "r");
    if(fPtr == NULL) return "-1";
    while(ch != EOF){
        ch = fgetc(fPtr);
        strncat(content,&ch,1);
    }
    fclose(fPtr);
    return content;
}

void sendToClient(char* response,int connection){
    if (send(connection,response,strlen(response),0) == -1){
        printf("Can't send response to the client\n");
        exit(1);
    }
}

char* createGetResponse(char* filePath){
    char *response = malloc(sizeof(char) * MAX_DATA_SIZE);
    char * content = readFile(filePath);
    if(strcmp(content,"-1") == 0) return FILE_NOT_FOUND_RESPONSE;
    strcat(response,OK_RESPONSE);
    strcat(response,content);
    return response;
}

char* createPostResponse(){
    return OK_RESPONSE;
}

void handleHTTPRequest(char* request,int connection){
    printf("request is \" %s \"\n",request);
    char *method,*uri,*version;
    method = strtok(request," ");
    uri = strtok(NULL," ");
    version = strtok(NULL,"\n");

    if(method == NULL || uri==NULL || version == NULL) sendToClient(METHOD_NOT_FOUND_RESPONSE,connection);
    else if (strcmp(method,"GET") == 0) sendToClient(createGetResponse(uri),connection);
    else if (strcmp(method,"POST") == 0) sendToClient(createPostResponse(),connection);
    else sendToClient (METHOD_NOT_FOUND_RESPONSE,connection);

}

int isEmptyLine(char *line){
    for(int i=0;i<strlen(line);i++) if(!isspace(line[i])) return 0;
    return 1;
}

int numOfEmptyLines(char *buffer){
    int emptyLines = 0;
    char * line;
    line = strtok (buffer,"\n");
    while (line != NULL) {
        if(isEmptyLine(line)) emptyLines++;
        else emptyLines = 0;
        line = strtok (NULL, "\n");
    }
    return emptyLines;
}

void* handleConnection(void* connection){
    int connectionDescriptor = *(int*)connection;
    printf("handeling connection %d\n",connectionDescriptor);
    char buffer[MAX_DATA_SIZE];
    int emptyLines = 0;
    //list of sockets to monitor events [only one socket in our case] 
    struct pollfd socketMonitor[1];
    socketMonitor[0].fd = connectionDescriptor;
    socketMonitor[0].events = POLL_IN;

    while(1){
        // poll if the socket had new event to handle or not.
        int numOfEvents = poll(socketMonitor,1, CONNECTION_TIMEOUT*1000);
        if(numOfEvents == 0) break; // no more IN events happend during the timeout interval
        
        char tempBuffer[MAX_DATA_SIZE];
        int receivedBytes = recv(connectionDescriptor,tempBuffer,MAX_DATA_SIZE-1,0);
        if(receivedBytes == 0) break; // the client closed the connection
        if(receivedBytes == -1){
            printf("Error when receiving from the client\n");
            exit(1);
        }
    
        tempBuffer[receivedBytes] = '\0'; //end of file
        strcat(buffer,tempBuffer);
        char tempCopy[MAX_DATA_SIZE];
        strcpy(tempCopy,buffer);
        emptyLines = numOfEmptyLines(tempCopy);
        if(emptyLines >= 1) {
            handleHTTPRequest(buffer,connectionDescriptor);
            emptyLines = 0;
            buffer[0] = '\0';
        }
    }
    printf("Connection %d timeout\n",connectionDescriptor);
    close(connectionDescriptor);
}

int main(int argc, char **argv){
    //Check the #arguments [it should be 2, 1st for the invocation command 2nd for port num]
    // if(argc != 2){
    //     printf("Invalid num of arguments\n");
    //     exit(1);
    // }
    // char *PORT_NUM = argv[1];
    char *PORT_NUM = "5051";
    struct addrinfo *info = getServerInfo(PORT_NUM);
    int socketDescriptor = createSocket(info);
    freeaddrinfo(info);
    listenOnSocket(socketDescriptor);
    printf("the server is up on port %s\n",PORT_NUM);
    
    while (1){
        int connection = acceptConnections(socketDescriptor);
        pthread_t thread;
        pthread_create(&thread, NULL, handleConnection, (void *)(&connection));
    }
    
    return 0;
}