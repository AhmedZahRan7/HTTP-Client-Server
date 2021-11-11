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
#define CONNECTION_TIMEOUT 1
#define MAX_DATA_SIZE 1000
#define MAX_FILE_SIZE 10000000
#define OK_RESPONSE "HTTP/1.1 200 OK\r\n\r\n"
#define FILE_NOT_FOUND_RESPONSE "HTTP/1.1 404 Not Found\r\n\r\n"
#define METHOD_NOT_FOUND_RESPONSE "HTTP/1.1 405 Method Not Allowed\r\n\r\n"

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

void sendStringToClient(char* response,int connection){
    int len = strlen(response);
    int sent = 0;
    int left = len;
    while (sent<len){
        int n = send(connection,response+sent,left,0);
        if (n == -1) break;
        sent += n;
        left -= n;
    }
}

void sendFileToClient(char* filePath,int connection){
    FILE *fPtr = fopen(filePath, "rb");
    if(fPtr == NULL) sendStringToClient(FILE_NOT_FOUND_RESPONSE,connection);
    sendStringToClient(OK_RESPONSE,connection);
    fseek(fPtr, 0, SEEK_END);
    int fileLen = ftell(fPtr);
    rewind(fPtr);
    fseek(fPtr, 0, SEEK_SET);
    int written = 0;
    char ch;
    while(written < fileLen){
        fread(&ch,1,1,fPtr);
        written += (send(connection,&ch,sizeof ch,0) != 0);
    }
    fclose(fPtr);
}

void handleHTTPRequest(char* request,int connection){
    printf("request is \" %s \"\n",request);
    char *method,*uri,*version;
    method = strtok(request," ");
    uri = strtok(NULL," ");
    version = strtok(NULL,"\n");
    if(method == NULL || uri==NULL || version == NULL) {
        sendStringToClient(METHOD_NOT_FOUND_RESPONSE,connection);
        return;
    }
    char baseURI[200] = "./welcome";
    strcat(baseURI,uri);
    if (strcmp(baseURI,"./welcome/") == 0) strcat(baseURI,"index.html");  
    if (strcmp(method,"GET") == 0 ) sendFileToClient(baseURI,connection);
    else if (strcmp(method,"POST") == 0) sendStringToClient(OK_RESPONSE,connection);
    else sendStringToClient (METHOD_NOT_FOUND_RESPONSE,connection);
}

char* replace(char* s,char* old,char* new){
    /*https://www.geeksforgeeks.org/c-program-replace-word-text-another-given-word/*/
    char* result;
    int i, cnt = 0;
    int newlen = strlen(new);
    int oldlen = strlen(old);

    for (i = 0; s[i] != '\0'; i++) {
        if (strstr(&s[i], old) == &s[i]) {
            cnt++;
              i += oldlen - 1;
        }
    }
    result = (char*)malloc(i + cnt * (newlen - oldlen) + 1);
    i = 0;
    while (*s) {
        if (strstr(s, old) == s) {
            strcpy(&result[i], new);
            i += newlen;
            s += oldlen;
        }
        else result[i++] = *s++;
    }
    result[i] = '\0';
    return result;
}

int isEmptyLine(char *line){
    if(strlen(line) == 0) return 1;
    for(int i=0;i<strlen(line);i++) if(!isspace(line[i])) return 0;
    return 1;
}

int numOfEmptyLines(char *b){
    char* buffer = replace(b,"\n"," \n");
    int emptyLines = 0;
    int lines = 0;
    char * line;
    line = strtok (buffer,"\n");
    while (line != NULL) {
        lines++;
        if(isEmptyLine(line)) emptyLines++;
        else emptyLines = 0;
        line = strtok (NULL, "\n");
    }

    //return -1 if the whole buffer is empty 
    return emptyLines==lines? -1 : emptyLines;
}

void* handleConnection(void* connection){
    int connectionDescriptor = *(int*)connection;
    printf("handeling connection %d\n",connectionDescriptor);
    char* buffer = (char*) malloc(sizeof(char) * MAX_DATA_SIZE);
    buffer[0] = '\0';
    int emptyLines = 0;
    //list of sockets to monitor events [only one socket in our case] 
    struct pollfd socketMonitor[1];
    socketMonitor[0].fd = connectionDescriptor;
    socketMonitor[0].events = POLLIN;

    while(1){
        // poll if the socket had new event to handle or not.
        int numOfEvents = poll(socketMonitor,1, CONNECTION_TIMEOUT*1000);
        if(numOfEvents == 0) break; // no more IN events happend during the timeout interval
        char *tempBuffer = (char*)malloc(sizeof(char) * MAX_DATA_SIZE);
        int receivedBytes = recv(connectionDescriptor,tempBuffer,MAX_DATA_SIZE,0);
        if(receivedBytes == 0) break; // the client closed the connection
        if(receivedBytes == -1){
            printf("Error when receiving from the client\n");
            exit(1);
        }
        tempBuffer[receivedBytes] = '\0'; //end of file
        strcat(buffer,tempBuffer);        
        char *tempCopy = (char*)malloc(sizeof(char) * MAX_DATA_SIZE);
        strcpy(tempCopy,buffer);
        emptyLines = numOfEmptyLines(tempCopy);
        if(emptyLines == -1) buffer[0] = '\0';
        if(emptyLines >= 1) {
            handleHTTPRequest(buffer,connectionDescriptor);
            emptyLines = 0;
            buffer[0] = '\0';
        }
        free(tempBuffer);
        free(tempCopy);
    }
    free(buffer);
    printf("Connection %d timeout\n",connectionDescriptor);
    close(connectionDescriptor);
}

int main(int argc, char **argv){
    //Check the #arguments [it should be 2, 1st for the invocation command 2nd for port num]
    if(argc != 2){
        printf("Invalid num of arguments\n");
        exit(1);
    }
    char *PORT_NUM = argv[1];
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