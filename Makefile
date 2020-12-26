# -*- tab-width:4 -*-

smtp_auth: smtp_auth.c account.h
	$(CROSS_TOOL)gcc -O1 -o $@ $<
	$(CROSS_TOOL)strip $@

account.h:
	@echo -n "input smtp username: " ; read user; \
	echo -n "input smtp password: "; read pass; \
	echo '#define BASE64_USER "'"`echo -n "$$user" | base64`"'"' >> $@; \
	echo '#define BASE64_PASS "'"`echo -n "$$pass" | base64`"'"' >> $@

ag300h:
	$(MAKE) CROSS_TOOL="mips-openwrt-linux-musl-" smtp_auth

clean:
	rm -f smtp_auth
