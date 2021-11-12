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

#define MAX_LINE_SIZE 1000
#define MAX_DATA_SIZE 10000000
#define CONNECTION_TIMEOUT .2
#define CLIENT_GET "get"
#define CLIENT_POST "post"
#define DEFAULT_PORT "80"
#define OK_RESPONSE "HTTP/1.1 200 OK"

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
    
    /*
        todo: getaddrinfo may take infinite time! [can be solved using conditional threads]
    */
    struct addrinfo *result;
    if (getaddrinfo(ip, port, &socketInformation, &result) != 0) {
        printf("can't get server information\n");
        return NULL;
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
    if(!info) return -1;
    int socketDescriptor = getValidDescriptor(info);
    if(socketDescriptor==-1) return -1;
    while(connect(socketDescriptor, info->ai_addr, info->ai_addrlen) == -1){
        close(socketDescriptor);
        socketDescriptor = getValidDescriptor(info);
    }
    if(info == NULL) {
        printf("can't connect to the server\n");
        return -1;
    }
    char s[INET6_ADDRSTRLEN];
    inet_ntop(info->ai_family, castToRightSocketAddress((struct sockaddr *)info->ai_addr),s, sizeof s);
    printf("\nopen connection %d with %s on port %s\n",socketDescriptor,s,port);
    freeaddrinfo(info);
    return socketDescriptor;
}

void sendString(char* response,int connection){
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

void sendFile(char* filePath,int connection){
    char path[100];
    path[0] = '.';
    path[1] = '\0';
    strcat(path,filePath);
    
    FILE *fPtr = fopen(path, "rb");
    if(fPtr == NULL) {printf("No such file %s\n",path);return;}
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

void writeToFile(char* filePath,char* data,int len){
    char path[100];
    path[0] = '.';
    path[1] = '\0';
    strcat(path,filePath);
    _mkdir(path);
    FILE *fPtr = fopen(path, "wb");
    char* begin = strstr(data,"\r\n\r\n");
    int headerLen = begin-data;
    for(int i=headerLen+4;i<len;i++) putc(data[i],fPtr);
    fclose(fPtr);
}

int receieveResponse(int connection,char* accumulatedBuffer){
    int connectionDescriptor = connection;
    //list of sockets to monitor events [only one socket in our case] 
    struct pollfd socketMonitor[1];
    socketMonitor[0].fd = connectionDescriptor;
    socketMonitor[0].events = POLLIN;
    int len = 0;
    while(1){
        // poll if the socket had new event to handle or not.
        int numOfEvents = poll(socketMonitor,1, CONNECTION_TIMEOUT*1000);
        if(numOfEvents == 0) return len; // no more IN events happend during the timeout interval
        char *buffer = (char*)malloc(sizeof(char) * MAX_DATA_SIZE);
        int receivedBytes = recv(connectionDescriptor,buffer,MAX_DATA_SIZE,0);
        if(receivedBytes == 0) {
            printf("Server closed the connection\n");
            free(buffer);
            return len;
        } 
        if(receivedBytes == -1){
            printf("Error when receiving from the client\n");
            free(buffer);
            return len;
        }
        for(int i=0;i<receivedBytes;i++) accumulatedBuffer[len+i] = buffer[i];
        len += receivedBytes;
        free(buffer);
    }
    return len;
}

int isEmptyLine(char *line){
    for(int i=0;i<strlen(line);i++) if(!isspace(line[i])) return 0;
    return 1;
}

void parseLine(void* line){
    char *method,*uri,*ip,*port;
    if(!(method = strtok((char*)line," "))) return;
    if(!(uri = strtok(NULL," "))) return;
    if(!(ip = strtok(NULL," "))) return;
    if(!(port = strtok(NULL,"\n"))){
        port = DEFAULT_PORT;
        ip[strlen(ip)-1]='\0';
    }
    int connection = connectToServer(ip,port);
    if(connection == -1) return;

    char* request = (char*)malloc(sizeof(char) * MAX_LINE_SIZE);
    request[0] = '\0';
    if(strcmp(method,CLIENT_GET)==0) {
        strcat(request,"GET ");
        strcat(request,uri);
        strcat(request," HTTP/1.1\r\n\r\n");
        sendString(request,connection);
        char *buffer = (char*)malloc(sizeof(char) * MAX_DATA_SIZE);
        int len = receieveResponse(connection,buffer) ;
        for(int i=0;i<len;i++) printf("%c",buffer[i]);
        char status[16];
        memcpy(status,buffer,15);
        status[15] = '\0';
        if(strcmp(OK_RESPONSE,status) == 0) writeToFile(uri,buffer,len);
        free(buffer);
    }
    else if(strcmp(method,CLIENT_POST)==0){
        strcat(request,"POST ");
        strcat(request,uri);
        strcat(request," HTTP/1.1\r\n\r\n");
        sendString(request,connection);
        sendFile(uri,connection);
        sendString("\n",connection);
        char *buffer = (char*)malloc(sizeof(char) * MAX_DATA_SIZE);
        int len = receieveResponse(connection,buffer);
        if(len) printf("%s",buffer);
        free(buffer);
    }
    else printf("%s not supported\n",method);
    free(request);
    close(connection);
}

int main(int argc, char **argv){
    if(argc != 2){
        printf("Invalid num of arguments\n");
        exit(1);
    }
    FILE* inputFile = fopen(argv[1],"r");
    if(!inputFile) {
        printf("Can't access input file\n");
        exit(1);
    }
    char* line = (char*)malloc(sizeof(char) * MAX_LINE_SIZE);
    while(fgets(line,MAX_LINE_SIZE,inputFile)) parseLine(line);
    free(line);
    fclose(inputFile);
    return 0;
}
