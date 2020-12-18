#!/usr/bin/perl

use Socket;
use Time::HiRes qw(sleep);

### サーバ
# 1. 受付用ソケットの作成
my $SockListen;
socket( $SockListen, PF_INET, SOCK_STREAM, getprotobyname( 'tcp' ))
	or die "Cannot create socket: $!";

setsockopt( $SockListen, SOL_SOCKET, SO_REUSEADDR, 1 );

# 2. 受付用ソケット情報の作成
my $pack_addr = sockaddr_in( 12345, INADDR_ANY );

# 3. 受付用ソケットと受付用ソケット情報を結びつける
bind( $SockListen, $pack_addr ) or die "Cannot bind: $!";

# 4. 接続を受け付ける準備をする。
listen( $SockListen, SOMAXCONN ) or die "Cannot listen: $!";

# 5. 接続を受け付けて応答する。
# 接続まち
accept( $Handle, $SockListen );
print "Connected\n";

# unbuffered
select( $Handle );
$| = 1;
select( STDOUT );

$Buf = '';

SendData( << '-----' );
220-XXXXXXXX.com ESMTP Exim 4.34
220-We do not authorize the use of this system to transport unsolicited,
220 and/or bulk e-mail.
-----

WaitCmd( 'EHLO' );

SendData( << '-----' );
250-XXXXXXXX.com Hello xsection.com [127.0.0.1]
250-SIZE 52428800
250-PIPELINING
250-AUTH PLAIN LOGIN
250-STARTTLS
250 HELP
-----

WaitCmd( 'AUTH' );

SendData( << '-----' );
334 ************
-----

WaitCmd( "\n" );

SendData( << '-----' );
334 ************
-----

WaitCmd( "\n" );

SendData( << '-----' );
235 Authentication successful
-----


while( 1 ){
	WaitCmd(); SendData( ">>>$Buf" ); $Buf = '';
}

WaitCmd( "\n" );

sub GetData {
	my( $param ) = @_;
	local( $_ );
	my( $tmp );
	
	if( $ConnMode == 2 ){
		sysread( $Handle, $_, 1 );
	}else{
		recv( $Handle, $_, 1024, defined( $param ) ? $param : 0 );
	}
	
	return $_ if( !$_ );
	$tmp = $_;
	
	s/([\x00-\x1F\[\x7E-\xFF])/sprintf( '[%02X]', ord( $1 ))/ge;
	print "Recv:$_\n";
	
	$tmp;
}

sub SendData {
	local( $_ ) = @_;
	my $ret;
	
	if( $ConnMode == 2 ){
		$ret = print $Handle $_;
	}else{
		$ret = send( $Handle, $_, 0 ) ? 1 : 0;
	}
	
	s/([\x00-\x1F\[\x7E-\xFF])/sprintf( '[%02X]', ord( $1 ))/ge;
	print "Send:$_\n";
}

sub WaitCmd {
	local( $_ ) = @_;
	
	print( "Waiting $_\n" );
	
	while( $Buf !~ /$_/ || $Buf !~ /[\x0D\x0A]/ ){
		print $Buf;
		$Buf .= GetData();
	}
	print( "OK\n" );
	
	$Buf = defined( $' ) ? $' : '';
}
