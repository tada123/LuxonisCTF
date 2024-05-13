

#include <unistd.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <cstdlib>
#include <errno.h>
#include <algorithm>
#include "protocol.hpp"

const size_t BUF_MAX = 16384;
char buf[BUF_MAX + 1]; //1 for NUL char
size_t bufpos = 0; //Buffer write pos
size_t bufcur = 0; //Buffer parsing pos

uint64_t chosenOpponentId = -1;
inline void resetBuf(){
    bufpos = 0;
    bufcur = 0;
}
void readNextData(int fd){
    size_t n = read(fd, buf + bufpos, BUF_MAX - bufpos);
    if(n < 0){
        perror("read");
        exit(5);
    }
    bufpos += n;
}
void* getNextMessage(MSGType* omsgt, psize_t* opayloadSz){
    const size_t msghdlen = sizeof(MSGType) + sizeof(psize_t);
    if((bufpos < (bufcur + msghdlen))){
        return NULL;
    }
    if((bufpos < (bufcur + msghdlen + (*(psize_t*) (buf + bufcur + sizeof(MSGType)))))){
        return NULL;
    }
    //Full message available
    *omsgt = (MSGType) *(buf + bufcur++);
    psize_t payloadSz = *(psize_t*) (buf + bufcur);
    *opayloadSz = payloadSz;
    
    bufcur += sizeof(psize_t);
    void* res = buf + bufcur;
    if((bufcur += payloadSz) >= bufpos){
        resetBuf();
    }
    return res;
}
void crash(const char* where){
    perror(where);
    exit(10);
}
void writeRequest(int fd, GameActionRequest reqtp, uint8_t* payload, psize_t npayload){
    write(fd, &reqtp, sizeof(reqtp));
    write(fd, &npayload, sizeof(npayload));
    write(fd, payload, npayload);
}
void askAndSendPasswordSync(int fd){
    printf("\n%s >>> ", "Please type username");
    char credentials[1024];
    if(!fgets(credentials, sizeof(credentials), stdin)){
        crash("fgets");
    }
    char* nl = strchr(credentials, '\n');
    if(nl){
        *nl = '\0';
    }
    size_t userSz = strlen(credentials) + 1;
    char* pass = &(credentials[userSz]);
    printf("\n%s >>> ", "Please type password");
    if(!fgets(pass, sizeof(credentials) - userSz, stdin)){
        crash("fgets");
    }
//    if(n < 0){
//        crash("getline");
//    }
    nl = strchr(pass, '\n');
    if(nl){
        *nl = '\0';
    }
    size_t credsz = (size_t) (pass + strlen(pass) - credentials);
    writeRequest(fd, garAuth, (uint8_t*) credentials, credsz);
}
void requestOpponentList(int fd){
    writeRequest(fd, garListOpponents, 0, 0);
}
void guessOneWord(int fd){
    printf("\n%s >>> ", "Try to guess a word");
    char buf[1024];
    if(!fgets(buf, sizeof(buf), stdin)){
        crash("fgets");
    }
    char* nl = strchr(buf, '\n');
    if(nl){
        *nl = '\0';
    }   
    writeRequest(fd, garGuessWord, (uint8_t*)buf, strlen(buf) + 1 /*Include NUL char*/);
}

uint64_t connectionPlayerId;
size_t guessAttempts; //Increased by server info response
int main(int argc, char** argv){
    if((argc < 3) || (strcmp(argv[1], "--help") == 0)){
        puts("client [connType] [addr]");
        puts("    connType:  TCP,UDS");
        puts("    addr:");
        puts("        TCP:  ip:port");
        puts("        UDS:  socketFilePath");
        return 1;
    }
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
        return 5;
    }
    char* addrstr = argv[2];
    sockaddr_storage addr;
    ((sockaddr_in*) &addr)->sin_family = fam;
    switch(fam){
        case AF_INET: {
            //Split by comma
            char* port = strchr(addrstr, ':');
            if(!port){
                fprintf(stderr, "%s\n", "ERROR: No port specified in address (Please write ip:port in second argument)");
                return 6;
            }
            *port = '\0';
            inet_pton(fam, addrstr, &((sockaddr_in*) &addr)->sin_addr);
            ((sockaddr_in*) &addr)->sin_port = htons(atoi(++port));
            break;
        }
        case AF_UNIX: {
            strcpy(((sockaddr_un*) &addr)->sun_path, argv[2]);
            break;
        }
    }
    if(connect(sock, (const sockaddr*)&addr, (fam == AF_UNIX) ? sizeof(sockaddr_un) : sizeof(sockaddr_in)) < 0){
        perror("connect");
        return 9;
    }
    
    bool currentUnsuccessfulAttempt = false;
    uint8_t gameStatus = 0; //0: Waiting for opponent   1: Guessing      2: Thinking a secret word
    uint8_t gameSubStatus = 0; // guessing {  }
    //Only use `poll` for single fd
    pollfd pf;
    pf.fd = sock;
    pf.events = POLLERR | POLLIN | POLLHUP | POLLRDHUP;
    while(1){
        int r = poll(&pf, 1, 20);
        if(r < 0){
            perror("poll");
            continue;
        }
        int ev = pf.revents;
        if(ev & POLLIN){
            MSGType mt;
            psize_t payloadSize;
            uint8_t* payload;
            readNextData(sock);
            while(payload = (uint8_t*)getNextMessage(&mt, &payloadSize)){
                switch(mt){
                    case msgtConnectionHandshake:
                        connectionPlayerId = *(uint64_t*) payload;
                        printf("%s\n", payload + sizeof(uint64_t));
                        askAndSendPasswordSync(sock);
                        break;
                    case msgtSuccessfulLogin: {
                        requestOpponentList(sock);
                        break;
                    }
                    case msgtOpponentsList: {
                        bool found = false;
                        uint8_t* endbuf = (uint8_t*)payload + payloadSize;
                        while(payload < endbuf){
                            if(*(payload + sizeof(uint64_t)) > 0){
                                //Opponent available
                                if(*(uint64_t*) payload != connectionPlayerId){
                                    if(!found){
                                        puts("Please choose opponent id from list: ");
                                        found = true;
                                    }
                                    printf("%d\n", *(uint64_t*) payload);
                                }
                            }
                            payload += sizeof(uint64_t) + 1;
                        }
                        if(!found){
                            puts("You are currently the only connected client, wait a minute for some opponent");
                            continue;
                        }
                        char buf[1024];
                        if(!fgets(buf, sizeof(buf), stdin)){
                            crash("fgets");
                        }
                        uint64_t id = atol(buf);
                        chosenOpponentId = id;
                        writeRequest(sock, garChooseOpponent, (uint8_t*)&id, sizeof(id));
                        break;
                    }
                    case msgtError: {
                        printf("Error: %s\n", ERROR_NAMES[*payload]);
                        switch(*payload){
                            case ecInvalidUsername:
                            case ecWrongPassword:
                                puts("Please try again (or press Ctrl-C to interrupt)");
                                askAndSendPasswordSync(sock);
                                break;
                            case ecOpponentAlreadyPaired:
                                requestOpponentList(sock);
                                break;
                        }
                        break;
                    }
                    case msgtOpponentChosen: { 
                        if(chosenOpponentId != *(psize_t*) payload){
                            puts("You were already chosen by another player before you typed the id, anyway you have a friend to play with :)");
                        }
                        bool guessing = (bool) *(payload + sizeof(psize_t));
                        printf("You're now %s player number %d\n", guessing ? "guessing a word from" : "writing a word for", *(psize_t*) payload);
                        if(guessing){
                            gameStatus = 2;
                        }else{
                            char buf[1024];
                            printf("Provide a secret word for player to guess >>> \e[1;33m"); //Colors!!!
                            fgets(buf, sizeof(buf), stdin);
                            printf("%s", "\e[0m");
                            char* nl = strchr(buf, '\n');
                            if(nl){
                                *nl = '\0';
                            }
                            writeRequest(sock, garProvideWord, (uint8_t*)buf, strlen(buf) + 1); //Include the NUL byte
                            gameStatus = 1;
                        }
                        break;
                    }
                    case msgtWordNotPreparedYet: {
                        puts("Wait a second, the opponent did't make up a word yet!");
                        break;
                    }
                    case msgtMessageFromOpponent: {
                        payload[payloadSize] = '\0'; //Better to prevent ugly buffer overflow
                        printf("Opponent's hint:\n%s\n", (const char*) payload);
                        break;
                    }
                    case msgtGuessed: {
                        switch(gameStatus){
                            case 1:
                                puts("Opponent guessed the word, that was a great game!!");
                                exit(0);
                                break; //Not really necessary, but it's just beautifufl
                            case 2:
                                puts("You guessed the word!!!");
                                usleep(5e5);
                                puts("Hope you will play again soon!");
                                exit(0);
                                break; //Not really necessary, but it's just beautifufl
                        }
                    }
                    case msgtUnsuccessfulAttempt: {
                        currentUnsuccessfulAttempt = true;
                        switch(gameStatus){
                            case 1: {
                                const char* suff[] = {"th", "st", "nd", "rd"};
                                uint8_t suffidx = ++guessAttempts % 10;
                                printf("Opponent's %d%s unsuccessful attempt.\n", guessAttempts, (suffidx < 4) ? suff[suffidx] : "th");
                                break;
                            }
                            case 2: {
                                puts("Incorrect, please try again");
                                break;
                            }
                        }
                    }
                }
            }
        }
        //Maybe nothing received, continue game
        switch(gameStatus){
            case 0:
                break; //Still waiting for opponent
            case 1:
                //Providing a word
                puts("Still waiting for opponent");
                if(currentUnsuccessfulAttempt){
                    currentUnsuccessfulAttempt = false;
                    printf("Opponent was wrong, write him a hint (Or leave empty to continue) >>> ");
                    char buf[1024];
                    fgets(buf, sizeof(buf), stdin);
                    if((*buf) && (*buf != '\n')){
//                        printf("Write a hint >>> ");
//                        fgets(buf, sizeof(buf), stdin);
                        char* nl = strchr(buf, '\n');
                        if(nl){
                            *nl = '\0';
                        }
                        writeRequest(sock, garSendTextToOpponent, (uint8_t*)buf, strlen(buf) + 1);
                    }
                }
                usleep(1e6);
                break;
            case 2:
                guessOneWord(sock);
                usleep(1e6);
                break;
                //Guessing a word
        }
    }
    
    
    
}
