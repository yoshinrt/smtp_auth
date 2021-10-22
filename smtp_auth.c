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
*/

#define PORT_SRC 25
#define PORT_DST 10025

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <signal.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>

#include "account.h"

#define DebugMsg( lv, fmt, ... )	if( g_uDebugLv >= lv ) printf( fmt, ##__VA_ARGS__ )

static inline int max( int a, int b ){
	return a > b ? a : b;
}

#define CloseSocket( fd ) if(( fd ) >= 0 ){ close( fd ); fd = -1; }

#define Case	break; case
#define Default	break; default

#define MAX_CONNECTION_CNT	3

typedef unsigned int	BOOL;
typedef unsigned int	UINT;

UINT g_uDebugLv = 1;

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

/*** buffer *****************************************************************/

#define BUF_SIZE	256
typedef struct {
	char	m_szBuf[ BUF_SIZE ];
	int 	m_iTail;
} tStrBuf;

int ReadBuf( tStrBuf *pStrBuf, int fd ){
	// リード
	int iSize = read( fd, pStrBuf->m_szBuf + pStrBuf->m_iTail, BUF_SIZE - pStrBuf->m_iTail );
	DebugMsg( 5, "rd: fd=%d size=%d\n", fd, iSize );
	
	if( iSize < 0 ){
		perror( "read" );
		return -1;
	}
	
	if( iSize ) pStrBuf->m_iTail += iSize;
	
	// FD_ISSET 通過 かつ size=0 はおかしいので -1 を返す
	return iSize > 0 ? iSize : -1;
}

#define WriteBufConst( str, fd ) WriteChar( str, fd, sizeof( str ) - 1 )

int WriteChar( const char *pBuf, int fd, int iSize ){
	// write
	int iWriteSize	= 0;
	
	if( g_uDebugLv >= 3 ){
		int i;
		printf( "%d> ", fd );
		for( i = 0; i < iSize; ++i ){
			char c = pBuf[ i ];
			if( c >= ' ' ) putchar( c );
			else printf( "[%02X]", c );
		}
		printf( "\n" );
	}
	
	while( iSize ){
		iWriteSize = write( fd, pBuf, iSize );
		DebugMsg( 5, "w%d\n", iWriteSize );
		if( iWriteSize < 0 ){
			perror( "write" );
			return -1;
		}
		pBuf	+= iWriteSize;
		iSize	-= iWriteSize;
	}
	
	return 1;
}

int WriteBuf( tStrBuf *pStrBuf, int fd, int iSize ){
	return WriteChar( pStrBuf->m_szBuf, fd, iSize );
}

int ReadLine( tStrBuf *pStrBuf ){
	// top から \n をサーチ
	int i;
	for( i = 0; i < pStrBuf->m_iTail; ++i ){
		if( pStrBuf->m_szBuf[ i ] == '\r' || pStrBuf->m_szBuf[ i ] == '\n' ){
			// \n が見つかったので改行スキップ
			for(; i < pStrBuf->m_iTail && ( pStrBuf->m_szBuf[ i ] == '\r' || pStrBuf->m_szBuf[ i ] == '\n' ); ++i );
			
			if( g_uDebugLv >= 3 ){
				int j;
				printf( "ReadLine>" );
				for( j = 0; j < i; ++j ){
					char c = pStrBuf->m_szBuf[ j ];
					if( c >= ' ' ) putchar( c );
					else printf( "[%02X]", c );
				}
				printf( "\n" );
			}
			
			return i;
		}
	}
	
	return 0;
}

void ShiftBuf( tStrBuf *pStrBuf, int iSize ){
	int i = 0;
	
	for(; iSize < pStrBuf->m_iTail; ++iSize ){
		pStrBuf->m_szBuf[ i++ ] = pStrBuf->m_szBuf[ iSize ];
	}
	
	pStrBuf->m_iTail = i;
}

int WriteLine( tStrBuf *pStrBuf, int fd, int iSize ){
	int iRet = WriteChar( pStrBuf->m_szBuf, fd, iSize );
	ShiftBuf( pStrBuf, iSize );
	
	return iRet;
}

int GetResponseCode( tStrBuf *pStrBuf ){
	int i;
	int iCode = 0;
	
	for( i = 0; i < pStrBuf->m_iTail; ++i ){
		if( pStrBuf->m_szBuf[ i ] == '-' ){
			iCode += 1000;
			break;
		}else if( isdigit( pStrBuf->m_szBuf[ i ])){
			iCode = iCode * 10 + pStrBuf->m_szBuf[ i ] - '0';
		}else{
			break;
		}
	}
	return iCode;
}

/*** tConnection class *****************************************************/

typedef struct {
	tStrBuf	m_SrcBuf;
	tStrBuf	m_DstBuf;
	
	int m_fdSrcSock;
	int m_fdDstSock;
	int m_iState;
} tConnection;

fd_set g_rfds;
fd_set g_rfdsActive;
int g_fdMax;
int g_iConnectionCnt	= 0;
BOOL g_bUpdate_fdMax	= 0;

//*** init struct

void InitConnection( tConnection *pConn ){
	pConn->m_iState			= S_OPENING;
	pConn->m_fdSrcSock		= -1;
	pConn->m_fdDstSock		= -1;
	pConn->m_SrcBuf.m_iTail	= 0;
	pConn->m_DstBuf.m_iTail	= 0;
}

//*** close connection

void Close( tConnection *pConn ){
	if( pConn->m_fdSrcSock >= 0 ) FD_CLR( pConn->m_fdSrcSock, &g_rfds );
	if( pConn->m_fdDstSock >= 0 ) FD_CLR( pConn->m_fdDstSock, &g_rfds );
	
	DebugMsg( 2, "del fds %d %d\n", pConn->m_fdSrcSock, pConn->m_fdDstSock );
	
	CloseSocket( pConn->m_fdSrcSock );
	CloseSocket( pConn->m_fdDstSock );
	--g_iConnectionCnt;
	g_bUpdate_fdMax = 1;
}

//*** new connection

int NewConnection( tConnection *pConn, int fdSockListen, int iDstPort ){
	
	struct sockaddr_in saClient;
	unsigned int uLen = sizeof( saClient );
	
	InitConnection( pConn );
	
	do{
		pConn->m_fdSrcSock = accept( fdSockListen, ( struct sockaddr *)&saClient, &uLen );
		if( pConn->m_fdSrcSock < 0 ){
			perror( "src accept" );
			break;
		}
		
		DebugMsg( 1, "opening dst port...\n" );
		// dest socket open
		struct sockaddr_in saDstAddr;
		memset( &saDstAddr, 0, sizeof( saDstAddr ));
		saDstAddr.sin_port			= htons( iDstPort );
		saDstAddr.sin_family		= AF_INET;
		saDstAddr.sin_addr.s_addr	= inet_addr( "127.0.0.1" );
		pConn->m_fdDstSock = socket( AF_INET, SOCK_STREAM, 0 );
		if( pConn->m_fdDstSock < 0 ){
			perror( "dst socket" );
			break;
		}
		
		int i = connect( pConn->m_fdDstSock, ( struct sockaddr *)&saDstAddr, sizeof( saDstAddr ));
		if( i < 0 ){
			perror( "dst connect" );
			break;
		}
		
		DebugMsg( 1, "start session\n" );
		
		// データが尽きるまでループ
		g_fdMax = max( max( pConn->m_fdSrcSock, pConn->m_fdDstSock ), g_fdMax - 1 ) + 1;
		FD_SET( pConn->m_fdSrcSock, &g_rfds );
		FD_SET( pConn->m_fdDstSock, &g_rfds );
		
		DebugMsg( 2, "add fds %d %d  fdMax=%d\n", pConn->m_fdSrcSock, pConn->m_fdDstSock, g_fdMax );
		
		++g_iConnectionCnt;
		return 0;
	}while( 0 );
	
	Close( pConn );
	return -1;
}

int ProcessMessage( tConnection *pConn, fd_set *pfdset ){
	
	int i;
	int iCode;
	int iRet = 0;
	
	while( 1 ){
		DebugMsg( 4, "State: %d\n", pConn->m_iState );
		
		switch( pConn->m_iState ){
			case S_OPENING:
				
				if(( i = ReadLine( &pConn->m_DstBuf )) == 0 ) return 0;
				
				iCode = GetResponseCode( &pConn->m_DstBuf );
				if( 200 <= iCode && iCode <= 299 ){
					pConn->m_iState = S_HELO;
				}
				
				if( WriteLine( &pConn->m_DstBuf, pConn->m_fdSrcSock, i ) < 0 ) return -1;
				if( 300 <= iCode && iCode < 1000 ) return -1;
				
			Case S_HELO:
				if(( i = ReadLine( &pConn->m_SrcBuf )) == 0 ) return 0;
				
				if(
					strncmp( pConn->m_SrcBuf.m_szBuf, "HELO", 4 ) == 0 ||
					strncmp( pConn->m_SrcBuf.m_szBuf, "EHLO", 4 ) == 0
				){
					pConn->m_iState = S_HELO_ACK;
				}
				
				if( WriteLine( &pConn->m_SrcBuf, pConn->m_fdDstSock, i ) < 0 ) return -1;
			
			Case S_HELO_ACK:
				
				if(( i = ReadLine( &pConn->m_DstBuf )) == 0 ) return 0;
				
				iCode = GetResponseCode( &pConn->m_DstBuf );
				if( 200 <= iCode && iCode <= 299 ){
					pConn->m_iState = S_AUTH;
				}
				
				if( WriteLine( &pConn->m_DstBuf, pConn->m_fdSrcSock, i ) < 0 ) return -1;
				if( 300 <= iCode && iCode < 1000 ) return -1;
			
			Case S_AUTH:
				#define STR_AUTH	"AUTH LOGIN\r\n"
				if( WriteBufConst( STR_AUTH, pConn->m_fdDstSock ) < 0 ) return -1;
				pConn->m_iState = S_AUTH_ACK;
			
			Case S_AUTH_ACK:
				
				if(( i = ReadLine( &pConn->m_DstBuf )) == 0 ) return 0;
				
				iCode = GetResponseCode( &pConn->m_DstBuf );
				if( 300 <= iCode && iCode <= 399 ){
					pConn->m_iState = S_USER;
				}
				
				ShiftBuf( &pConn->m_DstBuf, i );
				if( 400 <= iCode && iCode < 1000 ) return -1;
			
			Case S_USER:
				if( WriteBufConst( BASE64_USER "\r\n", pConn->m_fdDstSock ) < 0 ) return -1;
				pConn->m_iState = S_USER_ACK;
			
			Case S_USER_ACK:
				
				if(( i = ReadLine( &pConn->m_DstBuf )) == 0 ) return 0;
				
				iCode = GetResponseCode( &pConn->m_DstBuf );
				if( 300 <= iCode && iCode <= 399 ){
					pConn->m_iState = S_PASSWD;
				}
				
				ShiftBuf( &pConn->m_DstBuf, i );
				if( 400 <= iCode && iCode < 1000 ) return -1;
			
			Case S_PASSWD:
				if( WriteBufConst( BASE64_PASS "\r\n", pConn->m_fdDstSock ) < 0 ) return -1;
				pConn->m_iState = S_PASSWD_ACK;
			
			Case S_PASSWD_ACK:
				
				if(( i = ReadLine( &pConn->m_DstBuf )) == 0 ) return 0;
				
				iCode = GetResponseCode( &pConn->m_DstBuf );
				if( 200 <= iCode && iCode <= 299 ){
					pConn->m_iState = S_REPEAT;
				}
				
				ShiftBuf( &pConn->m_DstBuf, i );
				if( 300 <= iCode && iCode < 1000 ) return -1;
			
			Default:
				if( pConn->m_SrcBuf.m_iTail ){
					if( WriteBuf( &pConn->m_SrcBuf, pConn->m_fdDstSock, pConn->m_SrcBuf.m_iTail ) < 0 ) iRet = -1;
					pConn->m_SrcBuf.m_iTail = 0;
				}
				
				if( pConn->m_DstBuf.m_iTail ){
					if( WriteBuf( &pConn->m_DstBuf, pConn->m_fdSrcSock, pConn->m_DstBuf.m_iTail ) < 0 ) iRet = -1;
					pConn->m_DstBuf.m_iTail = 0;
				}
				return 0;
		}
	}
	return 0;
}

//*** repeater

int Repeater( tConnection *pConn ){
	int iRet = 0;
	
	// リード
	if(
		FD_ISSET( pConn->m_fdSrcSock, &g_rfdsActive ) &&
		ReadBuf( &pConn->m_SrcBuf, pConn->m_fdSrcSock ) < 0
	) iRet = -1;
	
	if(
		FD_ISSET( pConn->m_fdDstSock, &g_rfdsActive ) &&
		ReadBuf( &pConn->m_DstBuf, pConn->m_fdDstSock ) < 0
	) iRet = -1;
	
	if( ProcessMessage( pConn, &g_rfdsActive ) < 0 ) iRet = -1;
	
	return iRet;
}

/*** main *******************************************************************/

int main( int argc, char **argv ){
	int fdSockListen = -1;
	struct sockaddr_in saSrcAddr;
	
	int iArgPtr = 1;
	
	if( iArgPtr < argc && argv[ iArgPtr ][ 0 ] == '-' && argv[ iArgPtr ][ 1 ] == 'd' ){
		g_uDebugLv = atoi( argv[ iArgPtr ] + 2 );
		DebugMsg( 1, "Debug Lv = %d\n", g_uDebugLv );
		++iArgPtr;
	}
	
	int iSrcPort = PORT_SRC;
	int iDstPort = PORT_DST;
	
	if( iArgPtr < argc ){
		iDstPort = atoi( argv[ iArgPtr ]);
		++iArgPtr;
	}
	
	if( iArgPtr < argc ){
		iSrcPort = atoi( argv[ iArgPtr ]);
		++iArgPtr;
	}
	
	printf( "smth_auth: %d <-- %d\n", iDstPort, iSrcPort );
	
	signal( SIGPIPE, SIG_IGN );	/* シグナルを無視する */
	
	DebugMsg( 1, "opening listen port...\n" );
	
	fdSockListen = socket( AF_INET, SOCK_STREAM, 0 );
	if( fdSockListen < 0 ){
		perror( "src socket" );
		return 1;
	}
	
	saSrcAddr.sin_family = AF_INET;
	saSrcAddr.sin_port = htons( iSrcPort );
	saSrcAddr.sin_addr.s_addr = INADDR_ANY;
	
	if( bind( fdSockListen, ( struct sockaddr *)&saSrcAddr, sizeof( saSrcAddr )) != 0 ){
		perror( "src bind" );
		return 1;
	}
	
	if( listen( fdSockListen, 3 ) != 0 ){
		perror( "src listen" );
		return 1;
	}
	
	static tConnection Conn[ MAX_CONNECTION_CNT ];
	
	for( int i = 0; i < MAX_CONNECTION_CNT; ++i ){
		InitConnection( &Conn[ i ]);
	}
	
	FD_ZERO( &g_rfds );
	FD_SET( fdSockListen, &g_rfds );
	g_fdMax = fdSockListen + 1;
	
	DebugMsg( 1, "opening listen port done.\n" );
	
	// コネクション毎の処理
	while( 1 ){
		// リード g_fdDstSock コレクション
		g_rfdsActive = g_rfds;
		int i = select( g_fdMax, &g_rfdsActive, NULL, NULL, NULL );
		DebugMsg( 2, "select %d\n", i );
		
		if( i < 0 ){
			perror( "select" );
			break;
		}
		
		// 新規接続
		if( FD_ISSET( fdSockListen, &g_rfdsActive )){
			
			// Conn 空き検索
			int iFree;
			for( iFree = 0; iFree < MAX_CONNECTION_CNT; ++iFree ){
				if( Conn[ iFree ].m_fdSrcSock == -1 ) break;
			}
			
			if( iFree < MAX_CONNECTION_CNT ){
				DebugMsg( 1, "new connection %d\n", iFree );
				NewConnection( &Conn[ iFree ], fdSockListen, iDstPort );
			}else{
				DebugMsg( 1, "Max connection reached\n" );
				
				close( accept( fdSockListen, NULL, NULL ));
			}
		}
		
		for( int i = 0; i < g_iConnectionCnt; ){
			if( Conn[ i ].m_fdSrcSock != -1 ){
				// リピーター
				if( Repeater( &Conn[ i ] ) < 0 ){
					DebugMsg( 1, "end session %d\n", i );
					Close( &Conn[ i ] );
				}
				
				++i;
			}
		}
		
		// fdMax 再計算
		if( g_bUpdate_fdMax ){
			
			int iMax = fdSockListen;
			
			for( int i = 0, j = 0; i < MAX_CONNECTION_CNT && j < g_iConnectionCnt; ++i ){
				if( Conn[ i ].m_fdSrcSock != -1 ){
					iMax = max( max( Conn[ i ].m_fdSrcSock, Conn[ i ].m_fdDstSock ), iMax );
					++j;
				}
			}
			
			g_fdMax = iMax + 1;
			g_bUpdate_fdMax = 0;
			
			DebugMsg( 2, "dfMax = %d\n", g_fdMax );
		}
	}
	
	CloseSocket( fdSockListen );
	
	return 0;
}
