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
#include <sys/stat.h>
#include <semaphore.h>

#define MAX_WORKING_THREADS 5
#define MAX_WAITING_CONNECTIONS 5
#define MAX_DATA_SIZE 10000000
#define OK_RESPONSE "HTTP/1.1 200 OK\r\n\r\n"
#define FILE_NOT_FOUND_RESPONSE "HTTP/1.1 404 Not Found\r\n\r\n"
#define METHOD_NOT_FOUND_RESPONSE "HTTP/1.1 405 Method Not Allowed\r\n\r\n"
#define BASE_DIR "./base_dir"
#define DEFAULT_HTML "/index.html"
#define MAX_TIME_OUT 10 //in seconds

sem_t workingThreadsSemaphore; //semaphore that handles the maximum number of working threads [max connections I can handle atime]
pthread_mutex_t openConnectionMutex = PTHREAD_MUTEX_INITIALIZER; //to thread-safely change numOfOpenConnections value
int numOfOpenConnections = 0; //currently active connections
double connectionTimeout = MAX_TIME_OUT; //timeout assigned for new connection and is based on the num of active connections

void updateTimeOut(){
    connectionTimeout = numOfOpenConnections? MAX_TIME_OUT/numOfOpenConnections : MAX_TIME_OUT;
    printf("Currently handeling %d connections\nTimeout is %f seconds\n",numOfOpenConnections,connectionTimeout);
}
void increaseConnections(){
    pthread_mutex_lock(&openConnectionMutex);
    numOfOpenConnections++;
    updateTimeOut();
    pthread_mutex_unlock(&openConnectionMutex);
}
void decreaseConnections(){
    pthread_mutex_lock(&openConnectionMutex);
    numOfOpenConnections--;
    updateTimeOut();
    pthread_mutex_unlock(&openConnectionMutex);
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

//to accept one of waiting connections on that socket
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

//why sendfile() and sendstring() ? as the file may by binary so sending it as string will stop at first \0 charcater
void sendFileToClient(char* filePath,int connection){
    FILE *fPtr = fopen(filePath, "rb");
    if(fPtr == NULL) {
        sendStringToClient(FILE_NOT_FOUND_RESPONSE,connection);
        return;
    }
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

//recursuvely create directory
void _mkdir(char *dir) {
    /*http://nion.modprobe.de/blog/archives/357-Recursive-directory-creation.html*/
    char tmp[256];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp),"%s",dir);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = 0;
    for (p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, S_IRWXU);
            *p = '/';
        }
}

void writeToFile(char* filePath,char* content,int len){
    printf("len %d\n",len);
    _mkdir(filePath);
    FILE *fPtr = fopen(filePath, "wb");
    //decapsulate the header
    char* begin = strstr(content,"\r\n\r\n");
    int headerLen = begin-content;
    for(int i=headerLen+4;i<len;i++) putc(content[i],fPtr);
    fclose(fPtr);
}

void handleHTTPRequest(char* request,int len,int connection){
    int leadingSpaces = 0;
    while(isspace(request[leadingSpaces])) {
        leadingSpaces++;
        if(leadingSpaces == len) return;
    }
    char *duplication = (char*) malloc(sizeof(char)*len);
    printf("request is \n\"\n");
    for(int i=0;i<len-leadingSpaces;i++) {
        printf("%c",request[i+leadingSpaces]);
        duplication[i] = request[i+leadingSpaces];
    }
    printf("\n\"\n");
    char *method,*uri,*version;
    method = strtok(duplication," ");
    uri = strtok(NULL," ");
    version = strtok(NULL,"\n");

    if(method == NULL || uri==NULL || version == NULL) {
        sendStringToClient(METHOD_NOT_FOUND_RESPONSE,connection);
        return;
    }

    //base directory is the start directory of our sever 
    char baseURI[200] = BASE_DIR;
    //if the client not specified the file it needs so send it the default file
    if(strcmp(uri,"/") ==0) strcat(baseURI,DEFAULT_HTML);
    else strcat(baseURI,uri);

    if (strcmp(method,"GET") == 0 ) sendFileToClient(baseURI,connection);
    else if (strcmp(method,"POST") == 0) {
        sendStringToClient(OK_RESPONSE,connection);
        writeToFile(baseURI,request+leadingSpaces,len-leadingSpaces);
    }
    else sendStringToClient (METHOD_NOT_FOUND_RESPONSE,connection);
    free(duplication);
}

int isEmptyString(char* line,int len){
    for(int i=0;i<len;i++) if(!isspace(line[i])) return 0;
    return 1;
}
void* handleConnection(void* connection){
    increaseConnections();
    int connectionDescriptor = *(int*)connection;
    printf("handeling connection %d\n",connectionDescriptor);
    char* buffer = (char*) malloc(sizeof(char) * MAX_DATA_SIZE);
    int emptyLines = 0;
    //list of sockets to monitor events [only one socket in our case] 
    struct pollfd socketMonitor[1];
    socketMonitor[0].fd = connectionDescriptor;
    socketMonitor[0].events = POLLIN;
    int len = 0;
    // why while not just recv()? as no guarantee that recv() get all data in one call
    while(1){
        // poll if the socket had new event to handle or not.
        int numOfEvents = poll(socketMonitor,1, connectionTimeout*1000);
        if(numOfEvents == 0) break; // no more IN events happend during the timeout interval
        int receivedBytes = recv(connectionDescriptor,buffer+len,MAX_DATA_SIZE-len,0);
        if(receivedBytes == -1){
            printf("Error when receiving from the client\n");
            free(buffer);
            close(connectionDescriptor);
            decreaseConnections();
            sem_post(&workingThreadsSemaphore);
            return NULL;
        }
        if(receivedBytes == 0) break; // the client closed the connection
        len += receivedBytes;
        if(!isEmptyString(buffer,len) && len >= 4 && buffer[len-1] == '\n' && buffer[len-2] == '\r' && buffer[len-3] == '\n' && buffer[len-4] == '\r') {
            handleHTTPRequest(buffer,len,connectionDescriptor);
            len=0;
        }
    }
    if(len != 0) handleHTTPRequest(buffer,len,connectionDescriptor);
    free(buffer);
    printf("Connection %d timeout\n",connectionDescriptor);
    close(connectionDescriptor);
    decreaseConnections();
    sem_post(&workingThreadsSemaphore);
}

int main(int argc, char **argv){
    //Check the #arguments [it should be 2, 1st for the invocation command 2nd for port num]
    if(argc != 2){
        printf("Invalid num of arguments\n");
        exit(1);
    }
    sem_init(&workingThreadsSemaphore,0,MAX_WORKING_THREADS);
    char *PORT_NUM = argv[1];
    struct addrinfo *info = getServerInfo(PORT_NUM);
    int socketDescriptor = createSocket(info);
    freeaddrinfo(info);
    listenOnSocket(socketDescriptor);
    printf("the server is up on port %s\n",PORT_NUM);
    
    while (1){
        //block until the num of working threads < MAX_THREADS
        sem_wait(&workingThreadsSemaphore);
        int connection = acceptConnections(socketDescriptor);
        pthread_t thread;
        pthread_create(&thread, NULL, handleConnection, (void *)(&connection));
    }
    
    return 0;
}