#include <mysql/mysql.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_SIZE 1024
int temp;

void hand(char *query_string);
void retrieve(MYSQL* conn);
void Print(char *buf);


void Print(char *buf)
{
	char str[1024];
	strcpy(str,buf);
  	printf("<html>\n");
  	printf("<head>\n");
  	printf("<title>Show you option</title>\n");
  	printf("</head>\n");
	printf("<body>\n");
  	printf("<h1>");
  	printf("[%s]",str);
  	printf("</h1>\n");
  	printf("</body>\n");
  	printf("</html>\n"); 
}

//实现查询操作；
void retrieve(MYSQL* conn)
{
    int t,r; 
    MYSQL_RES *res;
    MYSQL_ROW row;
    char *buf = "select * from students";
    t=mysql_query(conn, buf);
    if(t)
	printf("Error making query:%s\n",mysql_error(conn));
    else
    {
	res=mysql_use_result(conn);
	if(res)
	{
		printf("<html>\n");
  		printf("<head>\n");
  		printf("<title>Show you option</title>\n");
  		printf("</head>\n");
		printf("<body>\n");
		printf("<h1> Let me show you result!</h1>\n");
		printf("<ul>\n");
		while((row=mysql_fetch_row(res))!=NULL)
		{	

			printf("<li>\n");
			for(t=0;t<mysql_num_fields(res);t++)
			{
				printf("the %dth data is %s;",t+1,row[t]); 
				printf("<br />");
			}
			printf("</li>\n");
		}
		printf("</ul>\n");
		printf("</body>\n");
  		printf("</html>\n");
	}
	mysql_free_result(res);
    }
    return;
}

void hand(char *query_string)
{
	int* mod=NULL;
	char buf[MAX_SIZE]={0};

	sscanf(query_string,"mod=%s",buf);
	temp=atoi(buf);	

	MYSQL* conn=mysql_init(NULL);
	mysql_real_connect(conn,"localhost","root","root","student",0,NULL,0);
	
    	switch(temp)
	{
	    case 1:
	    	//create(conn,name,age1,where);
	    	break;
	    case 2:
	    	//insert(conn,name,age1,where);
	    	break;
	    case 3:
		retrieve(conn);
		break;
	    case 4:
		//modify(conn,name,age1,where);
		break;
	    case 5:
		//delete(conn,name,where);
		break;
	    default:
		printf("error!\n");
		break;
	}

	mysql_close(conn);
}

int main()
{
	char query_string[MAX_SIZE]={0};
	if(getenv("REQUEST_METHOD"))
  {
    if(strcasecmp(getenv("REQUEST_METHOD"),"GET") == 0)
    {
      strcpy(query_string, getenv("QUERY_STRING"));
    }
    else
    {
     int content_length = atoi(getenv("CONTENT_LENGTH"));
     int i = 0 ;
     char c;
     
     for(; i < content_length; ++i)
     {
        read(0, &c, 1);
        query_string[i] = c;
     }
     query_string[i] = '\0';
    }
  }
	//char* query_string="mod=3";
	//Print(query_string);
	hand(query_string);
	return 0;
}

