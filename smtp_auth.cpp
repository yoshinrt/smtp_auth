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

#define DEBUG

#ifdef DEBUG
	#define DebugMsg( fmt, ... )	printf( fmt, ##__VA_ARGS__ )
#else
	#define DebugMsg( fmt, ... )
#endif

static inline int max( int a, int b ){
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

/*** buffer *****************************************************************/

#define BUF_SIZE	256
class CStrBuf {
public:
	char	m_szBuf[ BUF_SIZE ];
	int 	m_iTail;
	
	CStrBuf(){
		m_iTail = 0;
	}
	
	int WriteBuf( const char *pBuf, int fd, int iSize );
	int ReadBuf( int fd );
	
	int WriteBuf( int fd ){
		return WriteBuf2( m_szBuf, fd, m_iTail );
	}
	
	int WriteBuf( int fd, int iSize ){
		return WriteBuf2( m_szBuf, fd, iSize );
	}
	
	static int WriteBuf2( const char *pBuf, int fd, int iSize );
	
	int ReadLine( void );
	void ShiftBuf( int iSize );
	int WriteLine( int fd, int iSize );
	int GetResponseCode( void );
};

CStrBuf g_SrcBuf, g_DstBuf;

int CStrBuf::ReadBuf( int fd ){
	// リード
	int iSize = read( fd, m_szBuf + m_iTail, BUF_SIZE - m_iTail );
	//DebugMsg( "rd: fd=%d size=%d\n", fd, iSize );
	
	if( iSize < 0 ){
		perror( "read" );
		return -1;
	}
	
	if( iSize ) m_iTail += iSize;
	
	// FD_ISSET 通過 かつ size=0 はおかしいので -1 を返す
	return iSize > 0 ? iSize : -1;
}

#define WriteBufConst( str, fd ) CStrBuf::WriteBuf2( str, fd, sizeof( str ) - 1 )

int CStrBuf::WriteBuf2( const char *pBuf, int fd, int iSize ){
	// write
	int iWriteSize	= 0;
	
	#ifdef DEBUG
		int i;
		printf( "%d> ", fd );
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

int CStrBuf::ReadLine( void ){
	// top から \n をサーチ
	int i;
	for( i = 0; i < m_iTail; ++i ){
		if( m_szBuf[ i ] == '\r' || m_szBuf[ i ] == '\n' ){
			// \n が見つかったので改行スキップ
			for(; i < m_iTail && ( m_szBuf[ i ] == '\r' || m_szBuf[ i ] == '\n' ); ++i );
			
			#ifdef DEBUG
				int j;
				printf( "ReadLine>" );
				for( j = 0; j < i; ++j ){
					char c = m_szBuf[ j ];
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

void CStrBuf::ShiftBuf( int iSize ){
	int i = 0;
	
	for(; iSize < m_iTail; ++iSize ){
		m_szBuf[ i++ ] = m_szBuf[ iSize ];
	}
	
	m_iTail = i;
}

int CStrBuf::WriteLine( int fd, int iSize ){
	int iRet = WriteBuf2( m_szBuf, fd, iSize );
	ShiftBuf( iSize );
	
	return iRet;
}

int CStrBuf::GetResponseCode( void ){
	int i;
	int iCode = 0;
	
	for( i = 0; i < m_iTail; ++i ){
		if( m_szBuf[ i ] == '-' ){
			iCode += 1000;
			break;
		}else if( isdigit( m_szBuf[ i ])){
			iCode = iCode * 10 + m_szBuf[ i ] - '0';
		}else{
			break;
		}
	}
	return iCode;
}

/*** CConnection class *****************************************************/

class CConnection {
public:
	
	CStrBuf	m_SrcBuf;
	CStrBuf	m_DstBuf;
	
	int m_fdSrcSock;
	int m_fdDstSock;
	int m_iState;
	
	static fd_set m_rfds;
	static fd_set m_rfdsActive;
	static int m_fdMax;
	
	CConnection(){
		m_fdSrcSock	= -1;
		m_fdSrcSock	= -1;
		m_iState	= S_OPENING;
	}
	
	int NewConnection( int fdSockListen, int iDstPort );
	int Repeater( void );
	int ProcessMessage( fd_set *pfdset );
	void Close( void );
};

fd_set CConnection::m_rfds;
fd_set CConnection::m_rfdsActive;
int CConnection::m_fdMax;

//*** new connection

int CConnection::NewConnection( int fdSockListen, int iDstPort ){
	DebugMsg( "new connection\n" );
	
	struct sockaddr_in saClient;
	unsigned int uLen = sizeof( saClient );
	
	do{
		m_fdSrcSock = accept( fdSockListen, ( struct sockaddr *)&saClient, &uLen );
		if( m_fdSrcSock < 0 ){
			perror( "src accept" );
			break;
		}
		
		DebugMsg( "opening dst port...\n" );
		// dest socket open
		struct sockaddr_in saDstAddr;
		memset( &saDstAddr, 0, sizeof( saDstAddr ));
		saDstAddr.sin_port			= htons( iDstPort );
		saDstAddr.sin_family		= AF_INET;
		saDstAddr.sin_addr.s_addr	= inet_addr( "127.0.0.1" );
		m_fdDstSock = socket( AF_INET, SOCK_STREAM, 0 );
		if( m_fdDstSock < 0 ){
			perror( "dst socket" );
			break;
		}
		
		int i = connect( m_fdDstSock, ( struct sockaddr *)&saDstAddr, sizeof( saDstAddr ));
		if( i < 0 ){
			perror( "dst connect" );
			break;
		}
		
		DebugMsg( "start session\n" );
		
		// データが尽きるまでループ
		m_fdMax = max( max( m_fdSrcSock, m_fdDstSock ), m_fdMax - 1 ) + 1;
		FD_SET( m_fdSrcSock, &m_rfds );
		FD_SET( m_fdDstSock, &m_rfds );
		
		return 0;
	}while( 0 );
	
	Close();
	return -1;
}

//*** repeater

int CConnection::Repeater( void ){
	int iRet = 0;
	
	// リード
	if(
		FD_ISSET( m_fdSrcSock, &m_rfdsActive ) &&
		g_SrcBuf.ReadBuf( m_fdSrcSock ) < 0
	) iRet = -1;
	
	if(
		FD_ISSET( m_fdDstSock, &m_rfdsActive ) &&
		g_DstBuf.ReadBuf( m_fdDstSock ) < 0
	) iRet = -1;
	
	if( ProcessMessage( &m_rfdsActive ) < 0 ) iRet = -1;
	
	return iRet;
}

int CConnection::ProcessMessage( fd_set *pfdset ){
	
	int i;
	int iCode;
	int iRet = 0;
	
	while( 1 ){
		//DebugMsg( "State: %d\n", m_iState );
		
		switch( m_iState ){
			case S_OPENING:
				
				if(( i = g_DstBuf.ReadLine()) == 0 ) return 0;
				
				iCode = g_DstBuf.GetResponseCode();
				if( 200 <= iCode && iCode <= 299 ){
					m_iState = S_HELO;
				}
				
				printf( ">>>>%d %d\n", iCode, i );
				if( g_DstBuf.WriteLine( m_fdSrcSock, i ) < 0 ) return -1;
				if( 300 <= iCode && iCode < 1000 ) return -1;
				
			Case S_HELO:
				if(( i = g_SrcBuf.ReadLine()) == 0 ) return 0;
				
				if(
					strncmp( g_SrcBuf.m_szBuf, "HELO", 4 ) == 0 ||
					strncmp( g_SrcBuf.m_szBuf, "EHLO", 4 ) == 0
				){
					m_iState = S_HELO_ACK;
				}
				
				if( g_SrcBuf.WriteLine( m_fdDstSock, i ) < 0 ) return -1;
			
			Case S_HELO_ACK:
				
				if(( i = g_DstBuf.ReadLine()) == 0 ) return 0;
				
				iCode = g_DstBuf.GetResponseCode();
				if( 200 <= iCode && iCode <= 299 ){
					m_iState = S_AUTH;
				}
				
				if( g_DstBuf.WriteLine( m_fdSrcSock, i ) < 0 ) return -1;
				if( 300 <= iCode && iCode < 1000 ) return -1;
			
			Case S_AUTH:
				#define STR_AUTH	"AUTH LOGIN\r\n"
				if( WriteBufConst( STR_AUTH, m_fdDstSock ) < 0 ) return -1;
				m_iState = S_AUTH_ACK;
			
			Case S_AUTH_ACK:
				
				if(( i = g_DstBuf.ReadLine()) == 0 ) return 0;
				
				iCode = g_DstBuf.GetResponseCode();
				if( 300 <= iCode && iCode <= 399 ){
					m_iState = S_USER;
				}
				
				g_DstBuf.ShiftBuf( i );
				if( 400 <= iCode && iCode < 1000 ) return -1;
			
			Case S_USER:
				if( WriteBufConst( BASE64_USER "\r\n", m_fdDstSock ) < 0 ) return -1;
				m_iState = S_USER_ACK;
			
			Case S_USER_ACK:
				
				if(( i = g_DstBuf.ReadLine()) == 0 ) return 0;
				
				iCode = g_DstBuf.GetResponseCode();
				if( 300 <= iCode && iCode <= 399 ){
					m_iState = S_PASSWD;
				}
				
				g_DstBuf.ShiftBuf( i );
				if( 400 <= iCode && iCode < 1000 ) return -1;
			
			Case S_PASSWD:
				if( WriteBufConst( BASE64_PASS "\r\n", m_fdDstSock ) < 0 ) return -1;
				m_iState = S_PASSWD_ACK;
			
			Case S_PASSWD_ACK:
				
				if(( i = g_DstBuf.ReadLine()) == 0 ) return 0;
				
				iCode = g_DstBuf.GetResponseCode();
				if( 200 <= iCode && iCode <= 299 ){
					m_iState = S_REPEAT;
				}
				
				g_DstBuf.ShiftBuf( i );
				if( 300 <= iCode && iCode < 1000 ) return -1;
			
			Default:
				if( g_SrcBuf.m_iTail ){
					if( g_SrcBuf.WriteBuf( m_fdDstSock ) < 0 ) iRet = -1;
					g_SrcBuf.m_iTail = 0;
				}
				
				if( g_DstBuf.m_iTail ){
					if( g_DstBuf.WriteBuf( m_fdSrcSock ) < 0 ) iRet = -1;
					g_DstBuf.m_iTail = 0;
				}
				return 0;
		}
	}
	return 0;
}

//*** close socket

void CConnection::Close( void ){
	if( m_fdSrcSock >= 0 ) FD_CLR( m_fdSrcSock, &m_rfds );
	if( m_fdDstSock >= 0 ) FD_CLR( m_fdDstSock, &m_rfds );
	
	CloseSocket( m_fdSrcSock );
	CloseSocket( m_fdDstSock );
}

/*** main *******************************************************************/

int main( int argc, char **argv ){
	int fdSockListen = -1;
	struct sockaddr_in saSrcAddr;
	
	int iSrcPort = PORT_SRC;
	int iDstPort = PORT_DST;
	if( argc >= 2 ) iDstPort = atoi( argv[ 1 ]);
	if( argc >= 3 ) iSrcPort = atoi( argv[ 2 ]);
	printf( "smth_auth: %d <-- %d\n", iDstPort, iSrcPort );
	
	signal( SIGPIPE, SIG_IGN );	/* シグナルを無視する */
	
	DebugMsg( "opening listen port...\n" );
	
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
	
	CConnection Conn;
	FD_ZERO( &Conn.m_rfds );
	FD_SET( fdSockListen, &Conn.m_rfds );
	Conn.m_fdMax = fdSockListen + 1;
	
	DebugMsg( "opening listen port done.\n" );
	
	// コネクション毎の処理
	while( 1 ){
		// リード Conn.m_fdDstSock コレクション
		Conn.m_rfdsActive = Conn.m_rfds;
		int i = select( Conn.m_fdMax, &Conn.m_rfdsActive, NULL, NULL, NULL );
		DebugMsg( "select %d\n", i );
		
		if( i < 0 ){
			perror( "select" );
			break;
		}
		
		// 新規接続
		if( FD_ISSET( fdSockListen, &Conn.m_rfdsActive )){
			Conn.NewConnection( fdSockListen, iDstPort );
		}
		
		// リピーター
		if( Conn.Repeater() < 0 ){
			DebugMsg( "end session\n" );
			Conn.Close();
			
			Conn.m_fdMax = fdSockListen + 1;
		}
	}
	
	Conn.Close();
	CloseSocket( fdSockListen );
	
	return 0;
}
