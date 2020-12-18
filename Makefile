# -*- tab-width:4 -*-

smtp_auth: smtp_auth.c
	gcc -L$(XToolRoot)/mips/usr/lib -O1 -o $@ $<
	strip $@

clean:
	rm -f smtp_auth
