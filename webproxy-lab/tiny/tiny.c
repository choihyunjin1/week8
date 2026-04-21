/* $begin tinymain */
/*
 * tiny.c - A simple, iterative HTTP/1.0 Web server that uses the
 *     GET method to serve static and dynamic content.
 *
 * Updated 11/2019 droh
 *   - Fixed sprintf() aliasing issue in serve_static(), and clienterror().
 */
#include "csapp.h"
extern char **environ; // 프로그램 실행 환경 변수. CGI 프로그램을 실행할 때 인자를 넘겨주기 위해 전역으로 사용합니다.

void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg,
                 char *longmsg);

/*
 * main - 프로그램 진입점
 * [왜 필요한가요?] 웹 서버는 꺼지지 않고 계속 돌면서 클라이언트를 기다려야 합니다. 그 무한 대기의 뼈대를 만듭니다.
 * [어떻게 쓰이나요?] 특정 포트를 열고 연결 요청을 기다리다가, 손님이 오면 1:1 통신선(connfd)을 만든 뒤 doit()으로 넘깁니다. 
 * [어떻게 반영되나요?] 이 함수 덕분에 사용자가 브라우저에서 'localhost:8000'을 입력했을 때 서버가 즉각 반응하여 로직이 시작됩니다.
 */
int main(int argc, char **argv)
{
  int listenfd, connfd;
  char hostname[MAXLINE], port[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;

  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }

  /* 지정한 포트로 들어오는 요청을 감지할 '듣기 소켓'을 엽니다. */
  listenfd = Open_listenfd(argv[1]);
  
  while (1) /* 무한 루프로 서버가 꺼지지 않게 유지 */
  {
    clientlen = sizeof(clientaddr);
    
    /* Accept: 대기하다가 연결 요청이 오면 실제 통신을 위한 '연결 소켓(connfd)'을 생성합니다. */
    connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen); 
    
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
    printf("Accepted connection from (%s, %s)\n", hostname, port);
    
    /* 실질적인 HTTP 트랜잭션(요청 분석 + 응답 발송) 처리는 여기서 다 합니다. */
    doit(connfd);  
    
    /* 1:1 통신이 끝났으니 메모리 누수를 막기 위해 무조건 소켓을 닫아줍니다. */
    Close(connfd); 
  }
}

/*
 * doit - 하나의 HTTP 트랜잭션을 처리하는 핵심 두뇌 함수
 * [왜 필요한가요?] 소켓이 연결되었다고 끝이 아닙니다. 클라이언트가 "무엇을(어떤 파일을) 원하는지" 읽고 응답해야 합니다.
 * [어떻게 쓰이나요?] 클라이언트의 문자열(GET /home.html HTTP/1.1)을 읽어들인 뒤, 정적 파일인지 동적(수식 계산 등)인지 분류하고 적절한 목적지로 넘겨줍니다. 
 * [어떻게 반영되나요?] 클라이언트가 악성 요청을 하거나 파일이 없으면 여기서 걸러져서 404/403 에러가 전송되고, 정상 파일이면 serve_static/dynamic으로 무사히 넘어갑니다.
 */
void doit(int fd)
{
    int is_static;
    struct stat sbuf;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char filename[MAXLINE], cgiargs[MAXLINE];
    rio_t rio;

    Rio_readinitb(&rio, fd);
    /* 1. 요청 라인(예: "GET / HTTP/1.1") 읽기 */
    Rio_readlineb(&rio, buf, MAXLINE); 
    printf("Request headers:\n");
    printf("%s", buf);
    
    /* 문자열을 공백 단위로 쪼개어 메서드, URI, 버전을 변수에 담습니다. */
    sscanf(buf, "%s %s %s", method, uri, version);
    
    /* GET 요청이 아니면 501 에러를 응답하고 통신을 종료합니다. */
    if (strcasecmp(method, "GET")) {
        clienterror(fd, method, "501", "Not implemented", "Tiny does not implement this method");
        return;
    }
    
    /* 2. 요청 라인 밑에 따라붙는 부가 헤더들은 아직 안 쓰기 때문에 비워버리기 위해 호출합니다. */
    read_requesthdrs(&rio);

    /* 3. URI 분석: 이 요청이 정적 콘텐츠인지(1 리턴), 동적 프로그램인지(0 리턴) 판단합니다. */
    is_static = parse_uri(uri, filename, cgiargs);
    
    /* 4. 파일이 존재하는지 검사합니다. */
    if (stat(filename, &sbuf) < 0) {
        clienterror(fd, filename, "404", "Not found", "Tiny couldn't find this file");
        return;
    }

    if (is_static) { /* [정적 요청인 경우] */
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) { // 일반 파일이 맞는지, 읽기 권한은 있는지 체크
            clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't read the file");
            return;
        }
        /* 실제 파일을 클라이언트로 전송합니다. */
        serve_static(fd, filename, sbuf.st_size);
    }
    else { /* [동적 요청인 경우] */
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode)) { // 실행 권한이 있는지 체크
            clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't run the CGI program");
            return;
        }
        /* 동적 프로그램을 실행시킵니다. */
        serve_dynamic(fd, filename, cgiargs);
    }
}

/*
 * clienterror - 클라이언트에게 에러 내용을 시각화해서 보내는 함수
 * [왜 필요한가요?] 서버에 오류(파일 없음 404, 권한 없음 403 등)가 났을 때, 브라우저가 멈추는 게 아니라 "에러 났어요!"라는 안내창을 띄워주기 위해 필요합니다.
 * [어떻게 쓰이나요?] 상태 번호(errnum)와 에러 이유(cause)를 조합해 예쁜 HTML 문자열 뭉치를 만들어냅니다.
 * [어떻게 반영되나요?] 이 함수가 실행되면 클라이언트 브라우저 측에는 하얀 바탕에 "The Tiny Web server"라는 글자와 함께 자신이 저지른 에러 원인이 뜨게 됩니다.
 */
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) 
{
    char buf[MAXLINE], body[MAXBUF];

    /* 에러 페이지의 본문(HTML 내용) 작성 */
    /* 
     * ====================================================================
     * [과거 CS:APP 교재 원본 코드 주석 처리 - 필요 시 주석 해제하여 버그 재현]
     * ====================================================================
     */
    /*
    sprintf(body, "<html><title>Tiny Error</title>");
    sprintf(body, "%s<body bgcolor=\"ffffff\">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);
    */

    /*
     * ====================================================================
     * [수정된 코드 - Aliasing 버그 해결]
     * 자기 자신(body)을 읽으며 덮어쓰면(Undefined Behavior) 메모리가 깨집니다.
     * 따라서 body + strlen(body)로 끝점부터 이어붙이도록 바꿨습니다.
     * ====================================================================
     */
    sprintf(body, "<html><title>Tiny Error</title>");
    sprintf(body + strlen(body), "<body bgcolor=\"ffffff\">\r\n");
    sprintf(body + strlen(body), "%s: %s\r\n", errnum, shortmsg);
    sprintf(body + strlen(body), "<p>%s: %s\r\n", longmsg, cause);
    sprintf(body + strlen(body), "<hr><em>The Tiny Web server</em>\r\n");

    /* 에러 통보를 위한 HTTP 응답 '헤더' 부분 전송 */
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    
    /* 헤더 다음으로 실제 HTML 페이지 코드를 전송 */
    Rio_writen(fd, body, strlen(body));
}

/*
 * read_requesthdrs - HTTP 요청 버퍼 찌꺼기를 소모하는 청소부 함수
 * [왜 필요한가요?] 웹 브라우저는 단순히 목적지만 보내지 않고 User-Agent(어떤 브라우저 쓰는지) 등 수십 줄의 텍스트(헤더)를 같이 보냅니다. 이걸 네트워크 버퍼에서 강제로 퍼내주지 않으면 다음 통신이 심각하게 꼬여 망가집니다.
 * [어떻게 쓰이나요?] 문자열 끝을 알리는 빈 줄("\r\n")이 나타날 때까지 무식하게 읽어들여서 버립니다.
 * [어떻게 반영되나요?] 통신 통로가 깨끗해져서 다음에 응답을 보낼 때 안전하게 데이터를 전송할 수 있는 환경이 조성됩니다.
 */
void read_requesthdrs(rio_t *rp) 
{
    char buf[MAXLINE];

    Rio_readlineb(rp, buf, MAXLINE);
    /* "\r\n" 이 나올 때까지 반복해서 읽고 버립니다. */
    while(strcmp(buf, "\r\n")) {
        Rio_readlineb(rp, buf, MAXLINE);
        printf("%s", buf); 
    }
    return;
}

/*
 * parse_uri - URI 패스 분석기
 * [왜 필요한가요?] 주소창에 들어온 값이 단순한 사진인지(정적), 연산을 해달라는 지시인지(동적 프로그램 cgi-bin) 컴퓨터가 스스로 깨닫게 하기 위해 분리를 해줍니다.
 * [어떻게 쓰이나요?] uri에 "cgi-bin"이 없으면 정적, 있으면 동적 경로로 간주합니다. 그리고 서버의 로컬 기준 실제 파일 경로(filename) 문장을 여기서 완성시킵니다.
 * [어떻게 반영되나요?] 여기서 만들어진 filename은 이후 컴퓨터가 물리적인 디스크에서 실제 파일을 오픈(Open)할 때 사용하는 주소값으로 직결됩니다.
 */
int parse_uri(char *uri, char *filename, char *cgiargs)
{
    char *ptr;

    if (!strstr(uri, "cgi-bin")) {  /* 정적 콘텐츠라면 */
        strcpy(cgiargs, ""); 
        strcpy(filename, ".");  /* 로컬 현재 폴더(.)부터 시작해서 주소를 이어붙입니다. */
        strcat(filename, uri); 
        
        /* URI 끝이 '/' 슬래시면 자동으로 기본 페이지 띄워주기 구현 */
        if (uri[strlen(uri)-1] == '/')
            strcat(filename, "home.html");
        
        return 1; // 1은 정적을 의미
    }
    else { /* 동적 콘텐츠라면 */
        ptr = index(uri, '?'); /* 인자는 ? 기호 뒤에 붙어있습니다. "?a=1&b=2" 같은 형태 */
        if (ptr) {
            strcpy(cgiargs, ptr+1); /* cgiargs에 인자만 쏙 빼서 복사해넣습니다. */
            *ptr = '\0';            
        }
        else
            strcpy(cgiargs, ""); 
            
        strcpy(filename, ".");
        strcat(filename, uri);
        
        return 0; // 0은 동적을 의미
    }
}

/*
 * serve_static - 정적 파일 스트리밍 함수
 * [왜 필요한가요?] 존재하는 그림, HTML 파일 등을 클라이언트 측으로 실제로 전송해 줘야 하기 때문입니다.
 * [어떻게 쓰이나요?] HTTP 응답 헤더(200 OK 등) 세팅 후, 파일을 가상 메모리에 매핑(Mmap)한 뒤 소켓 쪽으로 쫙 흘려보냅니다.
 * [어떻게 반영되나요?] 브라우저가 화면을 그리는 데 필요한 모든 Raw 데이터(이미지 화소 정보 등)가 이 코드를 통해 전송되어, 사용자의 화면에 고질라 사진이나 글이 뜨게 됩니다.
 */
void serve_static(int fd, char *filename, int filesize)
{
    int srcfd;
    char *srcp, filetype[MAXLINE], buf[MAXBUF];

    /* 1. 이 파일이 무슨 형식인지 판별합니다. */
    get_filetype(filename, filetype); 
    
    /* 2. 클라이언트에게 파일 형식과 크기를 알리는 헤더를 보냅니다. */
    /* 
     * ====================================================================
     * [과거 CS:APP 교재 원본 코드 주석 처리 - 필요 시 주석 해제하여 버그 재현]
     * (이 부분이 브라우저가 "다시 줘!"라며 무한 요청 도배를 유발하는 주범입니다!)
     * ====================================================================
     */
    /*
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
    sprintf(buf, "%sConnection: close\r\n", buf);
    sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
    sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype); 
    Rio_writen(fd, buf, strlen(buf));
    */

    /*
     * ====================================================================
     * [수정된 코드 - Aliasing 버그 해결]
     * buf 배열을 자기 자신(%s)으로 덮어쓰면 C 최신 컴파일러에서 문자가 깨집니다.
     * 형식이 깨진 헤더를 받은 브라우저의 무한 재요청을 막기 위해
     * 끝포인터(buf + strlen(buf))를 계산해 안전하게 이어붙이도록 수정되었습니다.
     * ====================================================================
     */
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    sprintf(buf + strlen(buf), "Server: Tiny Web Server\r\n");
    sprintf(buf + strlen(buf), "Connection: close\r\n");
    sprintf(buf + strlen(buf), "Content-length: %d\r\n", filesize);
    sprintf(buf + strlen(buf), "Content-type: %s\r\n\r\n", filetype); 
    Rio_writen(fd, buf, strlen(buf));
    printf("Response headers:\n");
    printf("%s", buf);

    /* 3. 파일 내용 전송 */
    srcfd = Open(filename, O_RDONLY, 0); 
    /* Mmap: 하드 디스크에 있는 파일을 메모리로 바로 올립니다. (속도 월등히 빠름) */
    srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0); 
    Close(srcfd); 
    
    Rio_writen(fd, srcp, filesize); /* 메모리 내용을 소켓 네트워크로 밀어넣습니다. */
    
    Munmap(srcp, filesize); /* 다 보냈으면 누수 방지를 위해 메모리 연결 구조를 부숩니다. */
}

/*
 * get_filetype - MIME 타입 분류기
 * [왜 필요한가요?] 우리가 파일을 보낼 때 그림 파일을 "텍스트"라고 속여서 보내면 브라우저가 사진을 화면에 그리지 못하고 외계어로 출력해버립니다. 따라서 브라우저 스펙에 맞는 분류표를 던져줘야 합니다.
 * [어떻게 쓰이나요?] 문자열의 끝점 확장자를 보고 특정 단어를 복사(strcpy)해 돌려줍니다.
 * [어떻게 반영되나요?] 이 문자열이 결론적으로 Response 헤더의 "Content-type: text/html" 형태 안으로 쏙 들어가서 브라우저의 렌더링 방식을 좌지우지합니다.
 */
void get_filetype(char *filename, char *filetype)
{
    if (strstr(filename, ".html")) 
        strcpy(filetype, "text/html");
    else if (strstr(filename, ".gif")) 
        strcpy(filetype, "image/gif");
    else if (strstr(filename, ".png")) 
        strcpy(filetype, "image/png");
    else if (strstr(filename, ".jpg")) 
        strcpy(filetype, "image/jpeg");
    else
        strcpy(filetype, "text/plain"); 
}

/*
 * serve_dynamic - 동적 프로그램 구동기
 * [왜 필요한가요?] 1+1 = 2 라는 결과는 파일을 미리 만들어둘 수 없고 사용자가 요청했을 때 그때그때 내부 C 프로그램을 돌려서 도출해야 합니다. 즉, 서버 내부에서 "타 프로그램"을 실행시키고 결과물을 잡아채기 위해 필요합니다.
 * [어떻게 쓰이나요?] Fork로 자식을 낳아 자식에게 타 프로그램(cgi)을 실행시킵니다. 이때 자식의 모니터(표준 출력)를 클라이언트의 통신선(fd) 쪽으로 휘게 만들(Dup2)어, 프로그램의 결과가 곧바로 네트워크를 타도록 마법을 부립니다.
 * [어떻게 반영되나요?] 브라우저 화면에는 cgi 프로그램 내부에 적혀있던 printf("안녕 계산기야"); 구문이 마치 응답 웹페이지처럼 출력되어 나타나게 됩니다.
 */
void serve_dynamic(int fd, char *filename, char *cgiargs)
{
    char buf[MAXLINE], *emptylist[] = { NULL };

    /* 첫 번째 응답 헤더라인은 자체 서버가 대신 만들어 보냅니다. */
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Server: Tiny Web Server\r\n");
    Rio_writen(fd, buf, strlen(buf));

    /* Fork: 본체가 죽으면 안되므로, 부하(자식 프로세스)를 하나 파생시켜 걔한테 위험한 일을 다 맡깁니다. */
    if (Fork() == 0) { 
        
        /* CGI 프로그램이 기대하는 인자를 환경변수에 쑤셔넣어 줍니다. */
        setenv("QUERY_STRING", cgiargs, 1);
        
        /* Dup2 핵심: 자식 프로세스가 출력하는 결과(모니터, STDOUT)를 강제로 클라이언트 통신선(fd)으로 연결해버립니다! */
        Dup2(fd, STDOUT_FILENO);         
        
        /* 지정된 프로그램(수식 계산기 등)을 실행합니다. 이제부터 printf 시 클라이언트한테 전송됩니다. */
        Execve(filename, emptylist, environ); 
    }
    
    /* 부모(본체)는 자식이 출력(계산)을 다 끝내고 완전히 프로그램이 꺼질 때까지 여기서 대기합니다. 그래야 좀비가 생기지 않습니다. */
    Wait(NULL); 
}

/*
 * ============================================================================
 * [Tiny Web Server 동작 흐름 다이어그램 (함수 호출 구조)]
 * ============================================================================
 * 
 * [클라이언트 브라우저 접속] ---> main()
 *                                 │ (무한 대기하며 Accept로 연결 수락)
 *                                 ▼
 *                             doit() <----------------- (1:1 연결 성립 후 핵심 로직 시작)
 *                                 │
 *                                 ├─▶ read_requesthdrs() - 부가 HTTP 헤더들 읽고 버리기 (청소)
 *                                 │
 *                                 ├─▶ parse_uri() - URI 문자열 분석 (정적/동적 판별 및 경로 완성)
 *                                 │
 *                            [결과 분기]
 *                           /         \
 *                   정적 파일           동적 파일 (cgi-bin)
 *                  /                     \
 *                 ▼                       ▼
 *          serve_static()            serve_dynamic()
 *                 │                       │
 *                 ▼                       ▼
 *     get_filetype() - MIME 찾기      Fork() - 자식 프로세스 복제 생성
 *                 │                       ├─▶ (자식) [setenv] -> [Dup2] -> [Execve] -> CGI 실행
 *                 ▼                       └─▶ (부모) [Wait] -> 자식 프로세스 종료 대기
 *        Mmap() - 메모리 매핑
 *        Rio_writen() - 전송        (자식의 STDOUT 출력이 클라이언트 브라우저로 직접 쏴짐)
 *        Munmap() - 해제
 * 
 * ============================================================================
 * ※ 각 단계에서 검증 실패 시 (404 찾을 수 없음, 403 권한 없음 등):
 *    --> clienterror() 호출 후 에러 HTML 전송하고 즉시 통신 종료
 * ============================================================================
 */

/*
 * ============================================================================
 * [함수별 역할 및 필요성 총요약]
 * ============================================================================
 * 1. main (약 28번째 줄)
 *    - 무엇을 하나요? : 서버의 포트를 열고 클라이언트의 연결 요청을 무한 대기하며 수락(Accept)합니다.
 *    - 왜 필요한가요? : 웹 서버 프로그램이 종료되지 않고 24시간 내내 방문자를 맞이하기 위한 뼈대입니다.
 * 
 * 2. doit (약 66번째 줄)
 *    - 무엇을 하나요? : 실질적인 HTTP 요청(GET 등)을 읽고 분석하여 정적/동적 처리를 분기합니다.
 *    - 왜 필요한가요? : 클라이언트가 정확히 어떤 파일이나 수식을 원하는지 파악하고 알맞은 라인으로 토스해주는 브레인 역할이 반드시 필요하기 때문입니다.
 * 
 * 3. read_requesthdrs (약 149번째 줄)
 *    - 무엇을 하나요? : 요청 라인 이후에 따라오는 부가적인 HTTP 헤더들을 읽고 무시(버림)합니다.
 *    - 왜 필요한가요? : 브라우저가 보낸 남은 데이터(찌꺼기)를 네트워크 버퍼에서 비워주지 않으면, 이후 통신이 꼬여서 서버가 제대로 응답할 수 없습니다.
 * 
 * 4. parse_uri (약 167번째 줄)
 *    - 무엇을 하나요? : 주소(URI)를 분석해 정적 파일인지 cgi-bin 프로그램인지 판단하고 물리적인 서버 내 폴더 경로를 만듭니다.
 *    - 왜 필요한가요? : 브라우저가 보낸 주소 문자열만으로는 컴퓨터가 실제 디스크의 어디를 뒤져야 할지 알 수 없기 때문에 이를 번역하는 로직이 필수적입니다.
 * 
 * 5. serve_static (약 202번째 줄)
 *    - 무엇을 하나요? : 일반적인 물리 파일(HTML, 이미지 등)을 HTTP 응답으로 클라이언트에게 전송합니다.
 *    - 왜 필요한가요? : Mmap을 통해 거대한 파일을 메모리에 빠르게 올린 뒤, 소켓 통신망을 통해 브라우저로 쏴주어야 화면에 웹페이지가 출력될 수 있습니다.
 * 
 * 6. get_filetype (약 233번째 줄)
 *    - 무엇을 하나요? : 파일명 확장자(ex: .html, .gif)를 보고 알맞은 MIME 타입(text/html 등)을 반환합니다.
 *    - 왜 필요한가요? : 데이터만 보낸다고 끝나는 게 아니라 '이 데이터는 사진이야'라고 선언해 주어야 브라우저가 글자로 깨뜨리지 않고 올바른 그래픽으로 렌더링합니다.
 * 
 * 7. serve_dynamic (약 249번째 줄)
 *    - 무엇을 하나요? : 자식 프로세스를 생성(Fork)하여 별도의 CGI 프로그램을 실행시키고, 그 실행 결과를 통신선으로 직결시킵니다.
 *    - 왜 필요한가요? : 특정 연산의 결과값은 완성된 파일로 존재하지 않으므로, 요청이 들어온 순간에 타 프로그램을 구동시키고 그 모니터 출력(STDOUT)을 가로채서(Dup2) 보내야 합니다.
 * 
 * 8. clienterror (약 123번째 줄)
 *    - 무엇을 하나요? : 404, 403 같은 에러 상황 시 클라이언트 브라우저에 띄울 안내용 HTML 페이지를 즉석에서 조립해 보냅니다.
 *    - 왜 필요한가요? : 에러가 났을 때 아무 응답도 안 하면 브라우저는 무한 로딩을 돌게 되므로, "이러이러해서 실패했다"라고 규격화된 종료 선언을 해주기 위함입니다.
 * ============================================================================
 */

/*
 * ============================================================================
 * [실제 실행 시나리오 추적: 동적 파일(adder) 요청 시 흐름도]
 * ============================================================================
 * 시나리오: 사용자가 브라우저 주소창에 "http://localhost:8000/cgi-bin/adder?a=15&b=27" 입력
 * 
 * [네트워크 상태]
 * 클라이언트 IP = 127.0.0.1 (localhost)
 * 클라이언트 Port = 35512 (OS가 자동 할당한 임의 포트)
 * 서버 IP       = 127.0.0.1
 * 서버 Port     = 8000
 * 
 * [User Process: 브라우저]
 *  write(sockfd, "GET /cgi-bin/adder?a=15&b=27 HTTP/1.1\r\n...", ...)
 *         |
 *         v
 * [Kernel TCP/IP Stack] 
 *         | (데이터 전송)
 *         v
 * [Tiny Server: main] (listenfd로 대기 중)
 *         |
 *         | Accept() 완료 -> 클라이언트와 1:1로만 소통할 연결 소켓 식별자 획득 (가칭: connfd=4)
 *         | doit(connfd) 호출
 *         v
 * [Tiny Server: doit / parse_uri]
 *         | 1. HTTP 요청 문자열 수신
 *         | 2. "cgi-bin" 폴더 확인 -> "이건 동적 요청이군!" 결정
 *         | 3. 물음표 뒤의 파라미터 분리: cgiargs = "a=15&b=27"
 *         | serve_dynamic(connfd, "./cgi-bin/adder", "a=15&b=27") 토스
 *         v
 * [Tiny Server: serve_dynamic]
 *         | "200 OK" 상태 헤더를 브라우저로 선(先)전송
 *         |
 *         | Fork() 로 부모/자식 분리
 *      +--+----------------------------------+
 *      |  (부모 프로세스)                    |  (자식 프로세스)
 *      |  Wait(NULL)                       |  1. setenv("QUERY_STRING", "a=15&b=27") 실행
 *      |  -> 자식이 덧셈을 끝내고 죽을     |  2. Dup2(connfd, STDOUT_FILENO) 실행
 *      |     때까지 하염없이 기다림        |     -> 원래 모니터(1번)로 향하던 길을 꺾어, 네트워크 소켓(4번)으로 연결함!
 *      |                                   |  3. Execve("./cgi-bin/adder") 실행!
 *      v                                   v
 *   (대기 중)                        [adder.c 프로그램 진입]
 *                                          | getenv("QUERY_STRING") 호출 -> "a=15&b=27" 획득
 *                                          | a에 15, b에 27 파싱
 *                                          |
 *                                          | printf("Content-type: text/html\r\n\r\n") 호출
 *                                          | printf("... The answer is: 15 + 27 = 42 ...") 호출
 *                                          |
 *                                          | (자식은 모니터에 출력했다 생각하지만, 위에서 Dup2를 
 *                                          |  해뒀기 때문에 모든 글자가 소켓망을 타고 흘러감!)
 *                                          v
 *                             [Kernel TCP/IP Stack]
 *                                          |
 *                                          v
 *                               [User Process: 브라우저]
 *                       화면에 "15 + 27 = 42" 텍스트가 거짓말처럼 렌더링 됨!
 * 
 * ============================================================================
 */

/*
 * ============================================================================
 * [실제 실행 시나리오 추적: 정적 파일 기본접속(localhost:8000/) 시나리오]
 * ============================================================================
 * 시나리오: 사용자가 브라우저 주소창에 "http://localhost:8000/" 만 입력했을 때 고질라가 뜨는 이유
 * 
 * [네트워크 상태]
 * 클라이언트 IP = 127.0.0.1 (localhost)
 * 서버 IP       = 127.0.0.1
 * 서버 Port     = 8000
 * 
 * --- (1차 통신: HTML 문서 달라고 하기) ---
 * [User Process: 브라우저]
 *  write(sockfd, "GET / HTTP/1.1\r\n...", ...)
 *         |
 *         v
 * [Kernel TCP/IP Stack] (데이터 전송)
 *         v
 * [Tiny Server: parse_uri]
 *         | 1. '/' 만 들어왔네? "cgi-bin"이 없으니 정적 요청!
 *         | 2. 주소가 '/'로 끝나니까 뒤에 자동으로 "home.html"을 붙여줌!
 *         | 3. filename = "./home.html" 로 완성
 *         v
 * [Tiny Server: serve_static]
 *         | 1. "./home.html" 파일을 Mmap(메모리 매핑)으로 오픈
 *         | 2. 브라우저로 "text/html" 형식의 전체 데이터 전송 완료!
 *         v
 * [User Process: 브라우저]
 *  1. "home.html" 파일 내용 수신!
 *  2. 내용을 해석(파싱)하는 도중, 코드 안에 <img src="godzilla.gif"> 태그를 발견함!
 *  3. "어라? 이 사진은 나한테 없는데? 서버한테 달라고 해야겠다!" 
 *     -> 사용자 몰래 스스로 2차 요청을 바로 보냄!
 * 
 * --- (2차 통신: 고질라 사진 달라고 하기) ---
 * [User Process: 브라우저 스스로 자동 실행]
 *  write(sockfd, "GET /godzilla.gif HTTP/1.1\r\n...", ...)
 *         |
 *         v
 * [Tiny Server: parse_uri]
 *         | 1. '/godzilla.gif' 요청 수신!
 *         | 2. filename = "./godzilla.gif" 로 완성
 *         v
 * [Tiny Server: serve_static]
 *         | 1. get_filetype 으로 확장자가 gif니까 "image/gif" 다! 확인
 *         | 2. "./godzilla.gif" 파일을 Mmap 으로 오픈
 *         | 3. 브라우저로 "image/gif" 바이너리 데이터 전송 완료!
 *         v
 * [User Process: 브라우저]
 *  1. 서버가 보내준 고질라 이미지 바이너리 데이터를 수신!
 *  2. 아까 받아둔 1차 응답(home.html) 틀 안의 <img> 위치에 고질라 그래픽을 멋지게 렌더링!
 *  3. 사용자는 "http://localhost:8000/" 만 들어갔는데도 고질라 사진이 있는 페이지를 보게 됨!
 * 
 * ============================================================================
 */
