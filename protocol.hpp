
enum MSGType : uint8_t { //Response from server
    msgtCommand,
    msgtControl,
    msgtError, //Error message (Followed by 1byte error code payload)
    msgtConnectionHandshake, //Handshake + First message sent to setup the connection
    msgtMessage, //Message printed to screen
    msgtSuccessfulLogin,
    msgtOpponentsList, //List of opponents (Binary form)
    msgtOpponentChosen, //You have chosen opponent, or opponent chose you
    msgtMessageFromOpponent, //Opponent sent a text message
    msgtGaveUp, //Opponent gave up
    msgtUnsuccessfulAttempt, //Wrong guess
    msgtGuessed, //Opponent won
    msgtWordNotPreparedYet, //Opponent didn't make a word yet
};

enum GameActionRequest : uint8_t { //Request type
    garAuth, //Password
    garListOpponents,
    garChooseOpponent,
    garSendTextToOpponent,
    garGiveUp, //Give up
    garProvideWord,
    garGuessWord, //Try to guess a word
    
};

enum ErrorCode : uint8_t{
    ecLimitReached, //Max number of connections or buffer size reached
    ecNotAuthorized,
    ecInvalidUsername,
    ecWrongPassword,
    ecBadRequest, //Wrong request format 
    ecInvalidOpponentId,
    ecOpponentAlreadyPaired, 
    
    ecNotPairedYet, //Cannot guess (or provide) word, when not paired
    ecAnotherPosition, //guesser cannot provide a word, and vice-versa
};

const char* ERROR_NAMES[] = {"Limit reached", "Not authorized", "Invalid username", "Wrong password", "Bad request format", "Invalid opponent id specified", "Opponent already taken by another player", "You are not paired yet, thus cannot play", "Your game position does not allow doing this (probably client app internal error, rather than behavioral)"};

template<typename T> class Array {
public:
    T* buf;
    size_t _count;
    inline Array(){
        this->buf = NULL;
    }
    inline Array(size_t cnt){
        T* p = malloc(cnt * sizeof(T));
        if(!p){
            fprintf(stderr, "ERROR: Cannot allocate %d elements on heap. Exiting\n", cnt);
            exit(5);
        }
        this->buf = p;
        this->_count = cnt;
        T* ep = this->buf + cnt;
        for(; p < ep; p++){
            new (p) T();
        }
    }
    inline ~Array(){
        T* p = this->buf;
        if(p){
            size_t l = this->len();
            while(l){
                p[--l].~T();
            }
            free(p);
        }
    }
    inline size_t len(){
        return this->_count;
    }
    inline void resize(size_t newlen){
        void* re = realloc(this->buf, newlen);
        if(!re){
            fprintf(stderr, "ERROR: Cannot realloc %d elements. Exiting.\n", newlen);
        }
        this->buf = re;
    }
    inline T& operator[](size_t index){
        return this->buf[index];
    }
};

class String : public Array<char>{ //Only allocates a buffer (Not only it's not as ineffective as Array in high-level languages, but it's also more effective, than  char* with char-by-char comparison)
public:
    bool equals(String& s){
        size_t l = this->len();
        return (l == s.len()) && (memcmp(this->buf, s.buf, l) == 0);
    }
};
typedef uint64_t psize_t; //Protocol size_t (Deterministic size_t for 32bit and 64bit (or 123456bit machine))
inline size_t initMessage(uint8_t* outbuf, MSGType tp, psize_t payloadLen){
    (*(MSGType*) outbuf) = tp;
    (*(psize_t*) ++outbuf) = payloadLen;
    return sizeof(tp) + sizeof(payloadLen);
}

//ssize_t game_sendData(int fd, uint8_t* data, size_t sz){
//    size_t totalSent = 0;
//    
//    //Should run only once, but it's safer to check that
//    do{
//        ssize_t sent = write(a, sendbuf, currentSize);
//        if(sent < currentSize){
//            if(sent < 0){
//                perror("write");
//                return -1;
//            }else{
//                //Should not happen, as the `write` will block until all data sent successfully
//            }
//        }
//        totalSent += sent;
//    }while(totalSent < sz);
//    return totalSent;
//}
//ssize_t game_recvChunk(int fd, MSGType* omsgtype, psize_t* opayloadSz, void* obuf, size_t bufsz){
//    
//}








