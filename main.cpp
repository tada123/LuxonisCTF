

#include <unistd.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <cstdlib>
#include <errno.h>

void blaba(){
    //DBG
    void* x = malloc(5);
}
#include "protocol.hpp"

typedef struct Address {
    sockaddr_storage addr;
    
    inline sockaddr_in& getInet(){
        return *(sockaddr_in*) &this->addr;
    }
    inline sockaddr_un& getUnix(){
        return *(sockaddr_un*) &this->addr;
    }
    inline sockaddr* getGenericP(){
        return (sockaddr*) &this->addr;
    }
};

static const uint16_t SERVER_PORT = 4000;
static const size_t MAX_CLIENTS = 1024;
static const size_t CLIENT_BUFSZ = 4096;

uint64_t playerIdCounter = 0;
typedef struct ClientInfo {
    int fd;
    
    size_t paired; //Paired client index
    uint64_t id;
//    uint8_t state; //Game state
    bool authorized;
//    String id; //Long unique id (Currently set by client according to exam)
    
    size_t inpos; //In buffer pos
    uint8_t inbuffer[CLIENT_BUFSZ];
    size_t incursor; //Cursor for reading (When already loaded in buffer)
    
    size_t outpos; //Out buffer pos
    uint8_t outbuffer[CLIENT_BUFSZ];
    size_t outwpos; //Output writer pos
    
    void reset();
    inline uint8_t* getBufferAtCurrentPos(size_t* availableBytes){
        *availableBytes = CLIENT_BUFSZ - this->inpos;
        return this->inbuffer + this->inpos;
    }
    inline bool isAvailable(){
        return this->fd == -1;
    }
    inline bool isAssigned(){
        return this->fd > -1;
    }
    inline bool isPairingAvailable(){
        return this->paired != -1;
    }
};

ClientInfo clients[MAX_CLIENTS];

void ClientInfo::reset(){
    this->fd = -1;
    this->paired = -1;
//    this->state = 0;
    this->authorized = false;
    this->inpos = 0;
    this->outpos = 0;
    this->outwpos = 0;
    this->incursor = 0;
    this->id = playerIdCounter++; //Just initialize with random playerId (clientById will check, if fd assigned)
}
void init(){
    
}

size_t findAvailableClientIndex(){
    for(size_t i = 0; i < MAX_CLIENTS; i++){
        ClientInfo* c = &clients[i];
        if(c->isAvailable()){
            return i;
        }
    }
    return -1; //Returns (2^64)-1 anyway (Signed or unsigned does not matter there)
}
ssize_t findClientById(uint64_t id){
    size_t l = MAX_CLIENTS;
    while(l){
        ClientInfo* c = &clients[l];
        if(c->isAssigned() && c->id == id){
            return l;
        }
    }
    return -1;
}
void* client_prepareWriteData(ClientInfo* client, MSGType mt, psize_t payloadSize){
    size_t totalSz = sizeof(mt) + sizeof(payloadSize) + payloadSize;
    if((client->outpos + totalSz) > CLIENT_BUFSZ){
        fprintf(stderr, "%s\n", "Too much data sent to client, please send smaller chunks (This is a server error, which should never occur)");
        return NULL;
    }
    uint8_t* crbuf = (uint8_t*) client->outbuffer + client->outpos;
    (*crbuf++) = mt;
    (*(psize_t*) crbuf) = payloadSize;
    crbuf += sizeof(psize_t);
    client->outpos += sizeof(mt) + sizeof(psize_t);    
    return crbuf;
}
bool client_writeData(ClientInfo* client, MSGType mt, const uint8_t* payload, psize_t payloadSize){
    void* outPayload = client_prepareWriteData(client, mt, payloadSize);
    if(!outPayload){
        return false;
    }
    memcpy(outPayload, payload, payloadSize);
    client->outpos += payloadSize;
    return true;
}
void client_readNext(ClientInfo* client){
    size_t wavailable = sizeof(client->inbuffer) - client->inpos;
    if(wavailable == 0){
        // Memory could be allocated on heap and resized using `realloc`, but it should not be needed for this simple Word Game
        fprintf(stderr, "%s\n", "WARNING: Client sent bigger chunk, than currently allowed");
        return;
    }
    size_t nread = read(client->fd, client->inbuffer + client->inpos, wavailable);
    if(nread < 0){
        if(errno != EAGAIN){
            //Should not happen, but just for sure to prevent some undefined behavior
            perror("read");
            exit(17);
        }
    }else if(nread > 0){
        client->inpos += nread;
    }else{
        // End of stream without getting EPOLLHUP  ???
    }
}
uint8_t* client_getNextMessage(ClientInfo* c, GameActionRequest* outMsgType, psize_t* outPayloadSize){
    const size_t msghdlen = sizeof(MSGType) + sizeof(psize_t);
    if(c->inpos < msghdlen){
        return NULL;
    }
    size_t cur = c->incursor;
    uint8_t* buf = c->inbuffer + cur;
    size_t delta = (c->inpos - cur);
    if(delta < (msghdlen + *(psize_t*) (buf + sizeof(MSGType)))){
        //Not enough bytes
        return NULL;
    }
    //Got enough bytes!!!
    *outMsgType = (GameActionRequest)*(buf++);
    psize_t payloadSize = *(psize_t*) buf;
    *outPayloadSize = payloadSize;
    if((c->incursor += (msghdlen + payloadSize)) >= c->inpos){
        //No more bytes, reset the buffer pos (The returned data still valid until next read)
        c->incursor = 0;
        c->inpos = 0;
    }
    return buf;
    
}
int client_bufferWriteNext(ClientInfo* c){
    size_t ioCursor = c->outwpos; //Position of writing to fd (Current bytes already written to fd)
    size_t bufCursor = c->outpos; //Position of writing to buffer (Current buffer write position)
    ssize_t n = write(c->fd, c->outbuffer + ioCursor /*Written pos*/, bufCursor - ioCursor /*Needed to write*/);
    if(n < 0){
        //Unlikely
        perror("write");
        return n; //Better than just `return -1`, because compiler may optimize this not writing to eax(rax on x32) again (When already set by `write` function)
    }
    if((ioCursor += n) >= bufCursor /*Will not be higher than, but >= is a single operation, while == may take more ticks on older CPUs */){
        //All data from buffer written, reset the buffer to zero
        c->outwpos = 0;
        c->outpos = 0;
    }else{
        c->outwpos = ioCursor;
    }
    return 0;
}
void client_close(ClientInfo* c){
    close(c->fd);
    c->reset();
}
inline void client_writeError(ClientInfo* c, ErrorCode e){
    client_writeData(c, msgtError, (uint8_t*)&e, sizeof(e));
}
void client_writeErrorNow(ClientInfo* c, ErrorCode e){
    uint8_t errbuf[2];
    *errbuf = msgtError;
    *(errbuf + 1) = e;
    write(c->fd, errbuf, 2); //Yes, non-blocking => delivery not guaranted, but better than blocking
}
static int epollFd;
static const char* UDS_FILE = "/tmp/luxonis_uds.socket";

int main(int argc, char** argv){
    if((argc < 2) || (strcmp(argv[1], "--help") == 0)){
        printf("%s\n", "Usage:\nluxonis_game [type]\nTypes:\n    TCP | Network accessable socket (over TCP protocol)\n    UDS | Unix Domain Socket");
        return 1;
    }
    epollFd = epoll_create(16);
    
    int fam;
    if(strcmp(argv[1], "TCP") == 0){
        fam = AF_INET;
    }else if(strcmp(argv[1], "UDS") == 0){
        fam = AF_UNIX;
    }else{
        fprintf(stderr, "ERROR: Invalid socket type: \"%s\" - please see help for more information", argv[1]);
        return 5;
    }
    int sock = socket(fam, SOCK_STREAM, 0);
    if(sock < 0){
        perror("socket");
        return 8;
    }
    int reuse = 1;
    if(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0){
        fprintf(stderr, "WARNING: Cannot enable REUSEADDR on server socket due to: %s\n", strerror(errno));
        //Continue even when setsockopt failed
    }
    
    if(fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK) < 0){
        fprintf(stderr, "ERROR: Cannot enable non-blocing mode on server socket due to: %s\n", strerror(errno));
        return 8;
    }
    
    Address addr;
    
    //getInet() is CORRECT FOR BOTH CASES, as sin_family field has same offset for both addr types
    addr.getInet().sin_family = fam;
    if(fam == AF_INET){
        addr.getInet().sin_port = htons(SERVER_PORT);
        addr.getInet().sin_addr.s_addr = htonl(INADDR_ANY);
    }else{
        strcpy(addr.getUnix().sun_path, UDS_FILE);
    }
    if(bind(sock, addr.getGenericP(), sizeof(sockaddr_storage)) != 0){
        perror("bind");
        return 10;
    }
    
    if(listen(sock, 16) != 0){
        perror("listen");
        return 11;
    }
    epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLHUP | EPOLLERR |     EPOLLET /*Trigger only once per a real event (Don't trigger, if we just didn't write the fd yet )*/;
    ev.data.u64 = -1; //Use -1 for the listener socket
    if(epoll_ctl(epollFd, EPOLL_CTL_ADD, sock, &ev) != 0){
        perror("epoll_ctl");
        return 13;
    }
    
    const char welcomeMessage[] = "Hello, welcome there\nPlease enter your password:";
    const char notEnoughStorageMsg[] = "Sorry, but max number of users currently reached\ndisconnecting you\n";
    uint8_t sendbuf[2048]; //Buffer, when no client-specific buffer allocated 
    size_t currentSize;
    while(1){
        epoll_event recvevents[64];
        int r = epoll_wait(epollFd, recvevents, 64, -1);
        
        if(r == -1){
            perror("epoll_wait");
            continue;
            //May be safe to continue to next poll
        }
        while(r){ //No need to use additional 4 bytes for another iterator variable
            epoll_event* currentEvent = &(recvevents[--r]);
            int evflags = currentEvent->events; //Dereference only once (after the single currentEvent pointer calculation in previous step)
            if(currentEvent->data.u64 == -1){
                // Listener socket event, accept all pending connections
                if(evflags & EPOLLIN){
                    while(1){
                        struct sockaddr_storage clientAddr;
                        char astr[INET_ADDRSTRLEN];
                        socklen_t len = sizeof(clientAddr);
                        int a = accept(sock, (sockaddr*)&clientAddr, &len);
                        if(a < 0){
                            if(errno == EAGAIN){
                                break; //All connections accepted successfully
                            }else{
                                perror("accept");
                                return 16;
                            }
                        }
                        
                        //Make the connection non-blocking
                        if(fcntl(a, F_SETFL, fcntl(a, F_GETFL) | O_NONBLOCK) < 0){
                            perror("fcntl");
                            return 12;
                        }
                        
                        //Print target address in string form
                        if(fam == AF_INET){
                            inet_ntop(clientAddr.ss_family, &((sockaddr_in*) &clientAddr)->sin_addr, astr, sizeof(astr));
                            printf("Received connection from %s\n", astr);
                        }
                        size_t idx = findAvailableClientIndex();
                        if(idx < 0){
                            currentSize = initMessage(sendbuf, msgtError, sizeof(ErrorCode));
                            *(sendbuf + currentSize) = ecLimitReached;
//                            memcpy(sendbuf + currentSize, ecLimitReached, sizeof(ErrorCode));
//                            currentSize += sizeof(ErrorCode);
                            
                            //Send all in one step (Full delivery not guaranted, but still better, than just ungracefully close the conncetion or letting buffer overflow (to -1<=>((2^64)-1) index))
                            write(a, sendbuf, currentSize); 
                            close(a);
                            continue;
                        }
                        
                        ev.events = EPOLLIN | EPOLLOUT | EPOLLHUP | EPOLLERR |     EPOLLET /*Trigger only once per a real event (Don't trigger, if we just didn't write the fd yet )*/;
                        ev.data.u64 = idx;
                        if(epoll_ctl(epollFd, EPOLL_CTL_ADD, a, &ev) != 0){
                            perror("epoll_ctl");
                            return 13;
                        }
                        ClientInfo* c = &clients[idx];
                        c->reset(); //Reset for new connection
                        c->fd = a;
                    
                        //Send initial handshake
                        client_writeData(c, msgtConnectionHandshake, (const uint8_t*)welcomeMessage, sizeof(welcomeMessage));
//                        memcpy(crbuf, welcomeMessage, std::min(sizeof(welcomeMessage)));
//                        currentSize += sizeof(welcomeMessage);
                        
//                        client_writeData(client);
                    }
                }
                
                //Other events should not arise for server listener socket                
            }else{
                //Not using else-if, because we can receive multiple event types in a single poll
                ClientInfo* c = &clients[currentEvent->data.u64];
                int evflags = currentEvent->events; 
                if(evflags & (EPOLLHUP | EPOLLRDHUP)){
                    client_close(c);
                    continue; //Don't write to closed fd
                }
                if(evflags & EPOLLIN){
                    client_readNext(c);
                    GameActionRequest reqType;
                    psize_t payloadLen;
                    uint8_t* msgPayload;
                    while(msgPayload = client_getNextMessage(c, &reqType, &payloadLen)){
                        if(c->authorized){
                            switch(reqType){
                                case garListOpponents: {
                                    size_t i = 0;
                                    for(; i < MAX_CLIENTS; i++){
                                        if(!clients[i].isAssigned()){
                                            break;
                                        }
                                    }
                                    uint8_t* buf = (uint8_t*)client_prepareWriteData(c, msgtOpponentsList, i * (sizeof(uint64_t) + 1));
                                    if(!buf){
                                        client_writeErrorNow(c, ecLimitReached);
                                        continue;
                                    }
                                    for(size_t o = 0; o < i; o++){
                                        ClientInfo* iterc = &clients[o];
                                        (*(uint64_t*) buf) = iterc->id;
                                        buf += sizeof(uint64_t);
                                        *buf = (iterc->isPairingAvailable()) ? 1 : 0;
                                    }
                                }
                                case garChooseOpponent: {
                                    if(payloadLen != sizeof(uint64_t)){
                                        client_writeError(c, ecBadRequest);
                                        continue;
                                    }
                                    ssize_t idx = findClientById(*(uint64_t*) msgPayload);
                                    if(idx < 0){
                                        client_writeError(c, ecInvalidOpponentId);
                                    }
                                    if(c->isPairingAvailable()){
                                        c->paired = idx;
                                    }else{
                                        client_writeError(c, ecOpponentAlreadyPaired);
                                    }
                                }
                            }
                        }else{
                            if(reqType != garAuth){
                                client_writeErrorNow(c, ecNotAuthorized);
                                continue;
                            }
                        }
                        //Message available
                    }
                }
                if(evflags & EPOLLOUT){
                    client_bufferWriteNext(c);
                }
            }
        }
    }
}




