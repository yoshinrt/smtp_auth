/*
■SMTP-AUTH の仕組み
127.0.0.1...
Connected to localhost (127.0.0.1).
Escape character is '^]'.
220-XXXXXXXX.com ESMTP Exim 4.34 #1 ～～～
220-We do not authorize the use of this system to transport unsolicited,
220 and/or bulk e-mail.
EHLO xsection.com
250-XXXXXXXX.com Hello xsection.com [127.0.0.1]
250-SIZE 52428800
250-PIPELINING
250-AUTH PLAIN LOGIN
250-STARTTLS
250 HELP
AUTH LOGIN
334 ************
user id ←base64
334 ************
password ←base64
235 Authentication successful （成功）
535 Incorrect authentication data （失敗）

■base64 エンコード法: 以下の perl を実行
#!/usr/bin/perl
require 'mimew.pl';
&benflush( "qp" );
print( &bodyencode( 'user' ) . &benflush() . "\n" );
print( &bodyencode( 'pass' ) . &benflush() . "\n" );
*/

#define BASE64_USER	"your_base64_user"
#define BASE64_PASS	"your_base64_pwd"
#define PORT_SRC 10025
#define PORT_DST 10026

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <signal.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>

#define DEBUG

#ifdef DEBUG
	#define DebugMsg( fmt, ... )	printf( fmt, ##__VA_ARGS__ )
#else
	#define DebugMsg( fmt, ... )
#endif

static inline max( int a, int b ){
	return a > b ? a : b;
}

#define CloseSocket( fd ) if(( fd ) >= 0 ){ close( fd ); fd = -1; }

#define Case	break; case
#define Default	break; default

/*** smtp auth ステート *****************************************************/

enum {
	S_OPENING,
	S_HELO,
	S_HELO_ACK,
	S_AUTH,
	S_AUTH_ACK,
	S_USER,
	S_USER_ACK,
	S_PASSWD,
	S_PASSWD_ACK,
	S_REPEAT,
};

int g_iState = S_OPENING;

/*** buffer *****************************************************************/

#define BUF_SIZE	1024
typedef struct {
	char	szBuf[ BUF_SIZE ];
	int 	iTail;
} tStrBuf;
tStrBuf g_SrcBuf = { "", 0 }, g_DstBuf = { "", 0 };

int ReadBuf( tStrBuf *pBuf, int fd, fd_set *pfdset ){
	if( !FD_ISSET( fd, pfdset )) return 0;
	
	// リード
	int iSize = read( fd, pBuf->szBuf + pBuf->iTail, BUF_SIZE - pBuf->iTail );
	//DebugMsg( "rd: fd=%d size=%d\n", fd, iSize );
	
	if( iSize < 0 ){
		perror( "read" );
		return -1;
	}
	
	if( iSize ) pBuf->iTail += iSize;
	
	// FD_ISSET 通過 かつ size=0 はおかしいので -1 を返す
	return iSize > 0 ? iSize : -1;
}

int WriteBuf( char *pBuf, int fd, int iSize ){
	// write
	int iWriteSize	= 0;
	
	#ifdef DEBUG
		int i;
		printf( "%d:%d> ", g_iState, fd );
		for( i = 0; i < iSize; ++i ){
			char c = pBuf[ i ];
			if( c >= ' ' ) putchar( c );
			else printf( "[%02X]", c );
		}
		printf( "\n" );
	#endif
	
	while( iSize ){
		iWriteSize = write( fd, pBuf, iSize );
		//DebugMsg( "w%d\n", iWriteSize );
		if( iWriteSize < 0 ){
			perror( "write" );
			return -1;
		}
		pBuf	+= iWriteSize;
		iSize	-= iWriteSize;
	}
	
	return 1;
}

int ReadLine( tStrBuf *pBuf ){
	// top から \n をサーチ
	int i;
	for( i = 0; i < pBuf->iTail; ++i ){
		if( pBuf->szBuf[ i ] == '\r' || pBuf->szBuf[ i ] == '\n' ){
			// \n が見つかったので改行スキップ
			for(; i < pBuf->iTail && ( pBuf->szBuf[ i ] == '\r' || pBuf->szBuf[ i ] == '\n' ); ++i );
			
			#ifdef DEBUG
				int j;
				printf( "%d:ReadLine>", g_iState );
				for( j = 0; j < i; ++j ){
					char c = pBuf->szBuf[ j ];
					if( c >= ' ' ) putchar( c );
					else printf( "[%02X]", c );
				}
				printf( "\n" );
			#endif
			
			return i;
		}
	}
	
	return 0;
}

int ShiftBuf( tStrBuf *pBuf, int iSize ){
	int i = 0;
	
	for(; iSize < pBuf->iTail; ++iSize ){
		pBuf->szBuf[ i++ ] = pBuf->szBuf[ iSize ];
	}
	
	pBuf->iTail = i;
}

int WriteLine( tStrBuf *pBuf, int fd, int iSize ){
	WriteBuf( pBuf->szBuf, fd, iSize );
	ShiftBuf( pBuf, iSize );
}

/*** main *******************************************************************/

int GetResponseCode( tStrBuf *pBuf ){
	int i;
	int iCode = 0;
	
	for( i = 0; i < pBuf->iTail; ++i ){
		if( pBuf->szBuf[ i ] == '-' ){
			iCode += 1000;
			break;
		}else if( isdigit( pBuf->szBuf[ i ])){
			iCode = iCode * 10 + pBuf->szBuf[ i ] - '0';
		}else{
			break;
		}
	}
	return iCode;
}

int Repeater( int fdDst, int fdSrc, fd_set *pfdset ){
	
	int i;
	int iCode;
	int iRet = 0;
	
	while( 1 ){
		//DebugMsg( "State: %d\n", g_iState );
		
		switch( g_iState ){
			case S_OPENING:
				
				if(( i = ReadLine( &g_DstBuf )) == 0 ) return 0;
				
				iCode = GetResponseCode( &g_DstBuf );
				if( 200 <= iCode && iCode <= 299 ){
					g_iState = S_HELO;
				}
				
				printf( ">>>>%d %d\n", iCode, i );
				WriteLine( &g_DstBuf, fdSrc, i );
				if( 300 <= iCode && iCode < 1000 ) return -1;
				
			Case S_HELO:
				if(( i = ReadLine( &g_SrcBuf )) == 0 ) return 0;
				
				if(
					strncmp( g_SrcBuf.szBuf, "HELO", 4 ) == 0 ||
					strncmp( g_SrcBuf.szBuf, "EHLO", 4 ) == 0
				){
					g_iState = S_HELO_ACK;
				}
				
				WriteLine( &g_SrcBuf, fdDst, i );
			
			Case S_HELO_ACK:
				
				if(( i = ReadLine( &g_DstBuf )) == 0 ) return 0;
				
				iCode = GetResponseCode( &g_DstBuf );
				if( 200 <= iCode && iCode <= 299 ){
					g_iState = S_AUTH;
				}
				
				WriteLine( &g_DstBuf, fdSrc, i );
				if( 300 <= iCode && iCode < 1000 ) return -1;
			
			Case S_AUTH:
				#define STR_AUTH	"AUTH LOGIN\r\n"
				WriteBuf( STR_AUTH, fdDst, sizeof( STR_AUTH ) - 1 );
				g_iState = S_AUTH_ACK;
			
			Case S_AUTH_ACK:
				
				if(( i = ReadLine( &g_DstBuf )) == 0 ) return 0;
				
				iCode = GetResponseCode( &g_DstBuf );
				if( 300 <= iCode && iCode <= 399 ){
					g_iState = S_USER;
				}
				
				ShiftBuf( &g_DstBuf, i );
				if( 400 <= iCode && iCode < 1000 ) return -1;
			
			Case S_USER:
				WriteBuf( BASE64_USER "\r\n", fdDst, sizeof( BASE64_USER ) + 1 );
				g_iState = S_USER_ACK;
			
			Case S_USER_ACK:
				
				if(( i = ReadLine( &g_DstBuf )) == 0 ) return 0;
				
				iCode = GetResponseCode( &g_DstBuf );
				if( 300 <= iCode && iCode <= 399 ){
					g_iState = S_PASSWD;
				}
				
				ShiftBuf( &g_DstBuf, i );
				if( 400 <= iCode && iCode < 1000 ) return -1;
			
			Case S_PASSWD:
				WriteBuf( BASE64_PASS "\r\n", fdDst, sizeof( BASE64_PASS ) + 1 );
				g_iState = S_PASSWD_ACK;
			
			Case S_PASSWD_ACK:
				
				if(( i = ReadLine( &g_DstBuf )) == 0 ) return 0;
				
				iCode = GetResponseCode( &g_DstBuf );
				if( 200 <= iCode && iCode <= 299 ){
					g_iState = S_REPEAT;
				}
				
				ShiftBuf( &g_DstBuf, i );
				if( 300 <= iCode && iCode < 1000 ) return -1;
			
			Default:
				if( g_SrcBuf.iTail ){
					if( WriteBuf( g_SrcBuf.szBuf, fdDst, g_SrcBuf.iTail ) < 0 ) iRet = -1;
					g_SrcBuf.iTail = 0;
				}
				
				if( g_DstBuf.iTail ){
					if( WriteBuf( g_DstBuf.szBuf, fdSrc, g_DstBuf.iTail ) < 0 ) iRet = -1;
					g_DstBuf.iTail = 0;
				}
				return 0;
		}
	}
	return 0;
}

/*** main *******************************************************************/

int main( int argc, char **argv ){
	int fdSockListen = -1, fdSrcSock = -1, fdDstSock = -1;
	struct sockaddr_in saSrcAddr, saClient;
	int iLen;
	int i;
	fd_set rfds, rfds_tmp;
	struct termios ioOld, ioNew;
	
	int	iBaud = B9600;
	
	signal( SIGPIPE, SIG_IGN );	/* シグナルを無視する */
	
	DebugMsg( "opening listen port...\n" );
	
	fdSockListen = socket( AF_INET, SOCK_STREAM, 0 );
	if( fdSockListen < 0 ){
		perror( "src socket" );
		return 1;
	}
	
	saSrcAddr.sin_family = AF_INET;
	saSrcAddr.sin_port = htons( PORT_SRC );
	saSrcAddr.sin_addr.s_addr = INADDR_ANY;
	
	if( bind( fdSockListen, ( struct sockaddr *)&saSrcAddr, sizeof( saSrcAddr )) != 0 ){
		perror( "src bind" );
		return 1;
	}
	
	if( listen( fdSockListen, 3 ) != 0 ){
		perror( "src listen" );
		return 1;
	}
	
	DebugMsg( "opening listen port done.\n" );
	
	// コネクション毎の処理
	while( 1 ){
		
		DebugMsg( "waiting connection...\n" );
		
		iLen = sizeof( saClient );
		fdSrcSock = accept( fdSockListen, ( struct sockaddr *)&saClient, &iLen );
		if( fdSrcSock < 0 ){
			perror( "src accept" );
			break;
		}
		
		DebugMsg( "opening dst port...\n" );
		// dest socket open
		struct sockaddr_in saDstAddr;
		memset( &saDstAddr, 0, sizeof( saDstAddr ));
		saDstAddr.sin_port			= htons( PORT_DST );
		saDstAddr.sin_family		= AF_INET;
		saDstAddr.sin_addr.s_addr	= inet_addr( "127.0.0.1" );
		fdDstSock = socket( AF_INET, SOCK_STREAM, 0 );
		if( fdDstSock < 0 ){
			perror( "dst socket" );
			break;
		}
		
		i = connect( fdDstSock, ( struct sockaddr *)&saDstAddr, sizeof( saDstAddr ));
		if( i < 0 ){
			perror( "dst connect" );
			break;
		}
		
		DebugMsg( "start session\n" );
		
		// データが尽きるまでループ
		int fdMax = max( max( fdSrcSock, fdDstSock ), fdSockListen ) + 1;
		FD_ZERO( &rfds );
		FD_SET( fdSrcSock, &rfds );
		FD_SET( fdDstSock, &rfds );
		FD_SET( fdSockListen, &rfds );
		
		g_iState = S_OPENING;
		
		int bBreak = 0;
		while( !bBreak ){
			// リード fdDstSock コレクション
			rfds_tmp = rfds;
			i = select( fdMax, &rfds_tmp, NULL, NULL, NULL );
			
			if( i < 0 ){
				perror( "select" );
				break;
			}
			//DebugMsg( "sel_exit%d src:%d dst:%d listen:%d\n", i,
			//	FD_ISSET( fdSrcSock, &rfds_tmp ),
			//	FD_ISSET( fdDstSock, &rfds_tmp ),
			//	FD_ISSET( fdSockListen, &rfds_tmp )
			//);
			
			// リッスンソケットに接続があったら，現接続は切断
			if( FD_ISSET( fdSockListen, &rfds_tmp )){
				DebugMsg( "new connection\n" );
				break;
			}
			
			// リード
			if( ReadBuf( &g_SrcBuf, fdSrcSock, &rfds_tmp ) < 0 ) bBreak = 1;
			if( ReadBuf( &g_DstBuf, fdDstSock, &rfds_tmp ) < 0 ) bBreak = 1;
			if( Repeater( fdDstSock, fdSrcSock, &rfds_tmp ) < 0 ) bBreak = 1;
		}
		
		DebugMsg( "end session\n" );
		//tcsetattr( fdDstSock, TCSANOW, &ioOld );
		CloseSocket( fdDstSock );
		CloseSocket( fdSrcSock );
	}
	
	CloseSocket( fdDstSock );
	CloseSocket( fdSrcSock );
	CloseSocket( fdSockListen );
	
	return 0;
}
