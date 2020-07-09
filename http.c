#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <pthread.h>

#define MAX 1024
#define port 33234
#define threadsconut 3
#define MAIN_PATH "index.html"

//任务节点结构体；
typedef struct node
{
	void (*func)(void *arg);  //任务处理回调函数;
	void *arg;               //回调函数参数；
	struct node *next;
}TaskNode;

//线程池结构体；
typedef struct
{
	pthread_mutex_t lock;         //互斥锁；
	pthread_cond_t cond;          //条件变量；
	struct node *task_head;       //任务节点(始终指向第一个待处理任务)；
	pthread_t *thread_id;         //线程数组；
	unsigned int cur_queue_size;  //待处理的任务数目；
}pthreadPool;


//epoll红黑树，相当于树根，句柄，设置为全局的；
int epoll_fd = 0;
FILE* fp;
char* logfile="log.txt";
pthreadPool *pool = NULL;

void *thread_done(void *arg);
void ThreadInit();
int pthreadpool_add(void (*process)(void *arg),void *arg);
void Task_process(void *arg);

//日志初始化；
int init_log()
{
	fp=fopen(logfile,"a");
	if(fp==NULL)
	{
		printf("Log init is failed!\n");
		return -1;
	}
	return 0;
}

//错误记录到日志文件中，可变参数；
void Write_into_log(const char* fmt,...)
{
	//返回从1970开始的时间；
	time_t temptime=time(NULL);
	
	//使用tm结构来获得日期和时间，在头文件time.h中；
	//localtime：使用temptime的值来填充tm结构体；
	struct tm* cur_time=localtime(&temptime);
	if(!cur_time)
		return;
	char ArgBuf[MAX];
	//防止脏读；
	memset(ArgBuf,'\0',MAX);
	
	//使用strtime函数来格式化时间；
	//size_t strftime(char *str, size_t maxsize, const char *format, const struct tm *timeptr)
	//根据 format 中定义的格式化规则，格式化结构 timeptr 表示的时间，并把它存储在 str 中。
	//[%x,%X]:日期表示，时间表示；
	strftime(ArgBuf,MAX-1,"[%x %X]",cur_time);

	//将ArgBuf中的数据写回到fp中,每个元素的大小为1字节，因为是char类型；
	fwrite(ArgBuf,sizeof(ArgBuf),1,fp);

    va_list args;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);
    fprintf(fp, "\n");
    fflush(fp);  
}

//提取出一行数据；
int get_line(int sock, char* line, int size)
{
  	int i = 0;
  	char c = '\a';
	int s;
	
  	while((i < size) && (c != '\n'))
  	{
    	s = recv(sock, &c, 1 , 0);
		
		//没有数据，则退出；
    	if( s <= 0 )
      		break;
		
    	if(c == '\r')
    	{
      		recv(sock, &c, 1, MSG_PEEK);
      		if(c != '\n')
        		c = '\n';
      		else
        		recv(sock, &c, 1, 0);
    	}	
    	line[i] = c;
		i++;
 	}
  	line[i] = '\0';
  	return i;
}

//将请求头部信息全部读出来，丢弃，因为用不到；
void clear_header(int sock)
{
  	char buf[MAX];
  	do{
    	get_line(sock, buf, sizeof buf);
  	}while(strcmp(buf, "\n") != 0);
}

//CGI程序处理函数；
int exe_cgi(int sock, char* path, char* method, char* query_string)
{
  	//提取出method，query_string，content_length
  	char line[MAX];
  	int content_length = -1;

  	char method_env[MAX] = {0};
  	char query_string_env[MAX] = {0};
  	char content_length_env[MAX] = {0};

  	//如果是GET请求，读取并丢弃请求头部剩余信息；
  	if(strcasecmp(method , "GET") == 0)
    	clear_header(sock);
	
	//请求方法为POST；
  	else 
  	{
    	do
    	{
      		get_line(sock, line, sizeof(line));
			//一直循环读取出content-length字段为止；
      		if(strncmp(line, "Content-Length: ",16) == 0)
        		sscanf(line, "Content-Length: %d", &content_length);
    	}while(strcmp(line, "\n") != 0);
    
    	if(content_length == -1)
      		return 404;
  	}

  	//构造响应信息，先返回响应状态码200；
  	sprintf(line, "HTTP/1.1 200 OK\r\n");
  	send(sock, line, strlen(line), 0);
  	sprintf(line, "Content-type: text/html\r\n");
  	send(sock, line, strlen(line), 0);
  	sprintf(line, "\r\n");
  	send(sock, line, strlen(line), 0);

  	//使用管道进行交互，子进程和父进程；
  	int input[2];
  	int output[2];

	//创建两个管道；
 	if(pipe(input)<0)
 	{
 		Write_into_log("Input pipe is failed!\n");
		exit(1);
 	}
	Write_into_log("Input pipe is successful!\n");
	
  	if(pipe(output)<0)
  	{
		Write_into_log("Output pipe is failed!\n");
		exit(1);
 	}
	Write_into_log("Output pipe is successful!\n");

	//fork一个子进程用来执行cgi脚本；
  	pid_t pid = fork();
  	if(pid < 0)
  	{
  		Write_into_log("Fork a process is failed!\n");
		exit(1);
  	}

	//是子进程；
	//子进程需要使用新程序来替换其工作，因此要传递环境变量；
  	if(pid == 0)
  	{
  		Write_into_log("Child process is successful!\n");
  		//0代表stdin，1代表stdout
  		//关闭input的写端，关闭output的读端；
    	close(input[1]);
    	close(output[0]);

		//将标准输入重定向为input[0];
		//将标准输出重定向为output[1];
		//cgi是用标准输入输出来进行交互的；
    	dup2(input[0], 0);
    	dup2(output[1], 1);

		//设置REQUEST_METHOD请求方法环境变量；
    	sprintf(method_env, "REQUEST_METHOD=%s", method);
    	putenv(method_env);

		//是POST方法，需要存储CONTENT_LENGTH请求参数长度；
    	if(strcasecmp(method, "POST") == 0)
    	{
      		sprintf(content_length_env, "CONTENT_LENGTH=%d", content_length);
      		putenv(content_length_env);
    	}
		//是GET方法，需要存储QUERY_STRING请求参数；
    	else 
    	{
      		sprintf(query_string_env, "QUERY_STRING=%s", query_string);
      		putenv(query_string_env);
    	}
		
		//execl()用来执行参数path字符串所代表的文件路径;
		//接下来的参数代表执行该文件时传递过去的argv(0)、argv[1]……，最后一个参数必须用空指针(NULL)作结束;
    	execl(path, path, NULL);
  	}
	
	//父进程；
 	else 
  	{
  		Write_into_log("Father process is successful!\n");
    	//关闭input的读端，关闭output的写端；
    	close(input[0]);
    	close(output[1]);

    	char c;
		//要注意：
		//用户提交的数据是存储在CGI的标准输入中的，而不是在query_string；
		//所以我们之后在交互的时候，需要先获取数据长度，然后再从标准输入去读取数据；
    	if(strcasecmp(method, "POST") == 0)
    	{
      		for(int i=0;i<content_length; ++i)
      		{
      			//开始读取POST携带的数据，从sock读到c中；
        		read(sock, &c, 1);
				//将数据发送给cgi脚本；
        		write(input[1], &c, 1);
      		}
    	}
		
		//读取cgi脚本返回数据；
    	while(read(output[0], &c, 1) > 0)
      		send(sock, &c, 1, 0);

		//等待子进程结束回收其资源；
    	waitpid(input[1], 0, 0);
    	close(input[1]);
    	close(output[0]);
  	}
  	return 200;
}

//处理静态文件；
void echo_www(int sock, char* path, int size, int* errCode)
{
  	clear_header(sock);
  	char line[MAX];

  	int fd = open(path, O_RDONLY);
  	if(fd < 0)
  	{
    	Write_into_log("Open file is failed!\n");
    	exit(1);
  	}

  	sprintf(line, "HTTP/1.1 200 OK\r\n");
  	send(sock, line, strlen(line), 0);

  	sprintf(line, "Content-Length: %d\r\n",size);
  	send(sock, line, strlen(line), 0);
  
  	sprintf(line, "\r\n");
  	send(sock, line, strlen(line), 0);

  	sendfile(sock, fd, NULL, size);
  
  	close(fd);
}

//404 not found，页面不存在；
void echo_404(int sock)
{
	//将请求头部信息全部读出然后丢弃；
  	clear_header(sock);
  	char line[MAX];

  	const char* path = "wwwroot/404.html";

  	int fd = open(path,O_RDONLY);
  	if(fd < 0)
    	return ;
  	struct stat st;
  	stat(path, &st);
  	int size = st.st_size;

  	sprintf(line, "HTTP/1.1 200 OK\r\n");
  	send(sock, line, strlen(line), 0);

  	sprintf(line, "Content-Length: %d\r\n", size);
  	send(sock, line, strlen(line), 0);
  
  	sprintf(line, "\r\n");
  	send(sock, line, strlen(line), 0);

  	sendfile(sock, fd, NULL, size);
  
  	close(fd);
}

//套接字初始化
int socketInit()
{
  	//创建服务器套接字；
	int servfd = socket(AF_INET,SOCK_STREAM, 0);
	if( servfd < 0 )
  	{
    	Write_into_log("Socket is failed!\n");
    	exit(1);
  	}
	Write_into_log("Socket is successful!\n");

  	//端口可重用，解决time_wait的2MSL等待时间；
  	int opt = 1;
  	setsockopt(servfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

  	//进行端口号绑定
  	struct sockaddr_in serv;
	bzero(&serv,sizeof(serv));
  	serv.sin_family = AF_INET;
  	serv.sin_port = htons(port);
  	serv.sin_addr.s_addr = htonl(INADDR_ANY);
  	int res=bind(servfd, (struct sockaddr*)&serv, sizeof(serv));
	if(res!=0)
	{
    	Write_into_log("Bind is failed!\n");
    	exit(1);
  	}
	Write_into_log("Bind is successful!\n");
	
  	//设置监听上限；
  	if(listen(servfd, 5) < 0)
  	{
    	Write_into_log("Listen is failed!\n");
    	exit(1);
  	}
	Write_into_log("Listen is successful!\n");

  	return servfd;
}

//初始化线程池;
void ThreadInit()
{
	pool=(pthreadPool*)malloc(sizeof(pthreadPool));
	
	//初始化互斥锁和条件变量；
	pthread_mutex_init(&pool->lock,NULL);
	pthread_cond_init(&pool->cond,NULL);

	//向线程池中加入threadscount个线程；
	pool->thread_id=(pthread_t*)malloc(threadsconut*sizeof(pthread_t));
	pool->cur_queue_size=0;   //现在的任务队列为空；
	pool->task_head=NULL;     //要处理的任务节点；

	//创建的三个线程会阻塞在那边；
	for(int i=0;i<threadsconut;i++)
		pthread_create(&pool->thread_id[i],NULL,thread_done,NULL);
	
	Write_into_log("Threadpool init is successful!\n");
}

//向线程池中添加任务;
int pthreadpool_add(void (*process)(void *arg),void *arg)
{
	TaskNode* new_task=(TaskNode*)malloc(sizeof(TaskNode));
	new_task->func=process;
	new_task->arg=arg;
	new_task->next=NULL;

	pthread_mutex_lock(&pool->lock);
	//待处理任务数加1；
	pool->cur_queue_size++;

	//待处理任务的头结点(以链表形式组织);
	TaskNode* temp=pool->task_head;
	//为空则表示没有要处理的任务，那将当前添加的任务放入到待处理任务队列中；
	if(!temp)
		pool->task_head=new_task;
	//不为空，则需要将新添加的任务添加到任务队列尾部；
	else
	{
		while(temp->next)
			temp=temp->next;
		temp->next=new_task;
	}
	pthread_mutex_unlock(&pool->lock);
	//有任务，唤醒一个处理线程；
	pthread_cond_signal(&pool->cond);
	return 0;
}

//线程处理函数；
void *thread_done(void *arg)
{
	while(1)
	{
		//先进行加锁；
		pthread_mutex_lock(&(pool->lock));
		
		//如果没有任务要处理，当前线程阻塞(线程就是用来处理任务的);
		while(pool->cur_queue_size==0)
		{
			Write_into_log("No thread....Pending!\n");
			pthread_cond_wait(&pool->cond,&pool->lock);
		}

		//有任务要处理，则将任务数减1;
		pool->cur_queue_size--;
		//待处理任务的第一个
		TaskNode* worker=pool->task_head;
		//下一个要处理的任务;
		pool->task_head=worker->next;

		pthread_mutex_unlock(&(pool->lock));

		//调用函数执行任务,此时不需要锁;
		//worker->func是函数名，worker->arg是函数参数;
		(worker->func)(worker->arg);
		
		//任务执行完毕，释放内存，防止产生野指针；
		free(worker);
		worker=NULL;
	}
}

//用户执行的任务,自定义；
void Task_process(void *arg)
{
	int clifd=*(int*)arg;
	char first_line[MAX] = {0};
	//请求方法；
  	char method[MAX/32] = {0};  
	//请求URL；
  	char url[MAX] = {0};
	//请求路径；
  	char path[MAX] = {0};
	//请求参数；
  	char* query_string = NULL;
  	int errCode = 200;
	//是否需要执行cgi程序；
  	int cgi = 0;

  	int i=0; 
  	int j=0;

  	//获取请求头中的请求行；
  	if(get_line(clifd, first_line, sizeof(first_line)) ==  0 )
  	{
    	Write_into_log("Get one line is failed!\n");
		exit(1);
  	}
	Write_into_log("Get one line is successful!\n");
	
  	//在请求行中获取请求方法和请求url；
  	while(i < sizeof(method) -1 && j < sizeof(first_line) && !isspace(first_line[j]))
  	{
   	   method[i] = first_line[j];
	   i++;
	   j++;
  	}
  	method[i] = '\0';

	while(j < sizeof(first_line) && isspace(first_line[j]))
		j++;
	
  	i = 0;
  	while(i < sizeof(url) -1 && j < sizeof(first_line) && !isspace(first_line[j]))
  	{
      	url[i] = first_line[j];
		i++;
		j++;
  	}

  	//判断请求方法,
  	//是POST方法，无需取出query_string，存到环境变量，之后直执行cgi；
  	//是GET方法，需要取出query_string，进行参数解析；
  	if(strcasecmp(method, "POST") == 0)
    	cgi = 1;
	
	//是GET方法；
  	else if(strcasecmp(method, "GET") == 0 )
  	{
    	query_string = url;
    	while(*query_string)
    	{
    		//GET方法携带参数，执行cgi;
      		if(*query_string == '?')
      		{
        		*query_string = '\0';
        		query_string++;
        		cgi = 1;
        		break;
      		}
      		query_string++; 
    	}
  	}
  	else 
  	{
    	//其他请求方法；
    	errCode = 402;
		
		//跳转到标记为end的地方执行程序；
    	goto end;
  	}

  	//请求行已经解析完毕；
  	//将文件路径格式化到path中；
  	sprintf(path, "wwwroot%s", url);

	//如果结尾是'/',则是访问主页index.html；
  	if(path[strlen(path)-1] == '/')
    	strcat(path, MAIN_PATH);

  	//利用stat函数，通过文件名来获取文件信息；
  	//成功返回0，失败返回-1；
  	struct stat st;

	//需要访问的网页不存在；
  	if(stat(path, &st) < 0)
  	{
    	errCode = 403;
    	goto end;
  	}
	
	//访问的网页存在，则继续执行；
  	if(S_ISDIR(st.st_mode))
    	//如果是目录,则显示主页；
    	strcat(path, MAIN_PATH);

	//S_IXUSR:文件所有者具可执行权限；
	//S_IXGRP:用户组具可执行权限；
    //S_IXOTH:其他用户具可读取权限； 
  	if((st.st_mode & S_IXUSR)||(st.st_mode & S_IXGRP )||(st.st_mode & S_IXOTH))
    	cgi = 1;

	//cgi==1,说明有参数，不能返回静态页面，需要执行cgi程序；
  	if(cgi)
	{
		Write_into_log("CGI=1 is executing\n");
		//需要判断返回值，来看是否正确执行了；
    	errCode = exe_cgi(clifd, path, method, query_string);
	}
	//cgi==0,返回静态页面;
  	else 
	{
		Write_into_log("CGI=0 is executing\n");
    		echo_www(clifd, path, st.st_size, &errCode);
	}

end:
  	if(errCode != 200 )
    	echo_404(clifd);
  	close(clifd);
}

//主函数；
int main()
{
	//初始化日志；
	int out=init_log();
	if(out<0)
		Write_into_log("Log init is failed!\n");
	Write_into_log("Log init is successful!\n");
	
	ThreadInit();
	sleep(4);
	
	//创建服务端监听套接字；
  	int lisfd = socketInit();
	if(lisfd<0)
	{
		//记录日志；
		Write_into_log("SocketInit is failed!\n");
		return -1;
	}
	Write_into_log("SocketInit is successful!\n");

  	while(1)
  	{
  		//服务端接收连接请求；
    	int clifd=accept(lisfd,NULL,NULL);
		if(clifd==-1)
		{
			Write_into_log("Accept is failed!\n");
			exit(1);
		}
		Write_into_log("Acceot is successfully！\n");
		
		pthreadpool_add(Task_process,&clifd);	
  	}
 	return 0;
}

//首先绑定地址结构等，服务器开始连接客户端，一个客户端就代表一个任务；
//将任务加入到任务队列中，当任务队列中有有任务的时候就唤醒阻塞在；
//条件变量上的一个线程来对任务进行处理，处理完后在回到线程池中；
