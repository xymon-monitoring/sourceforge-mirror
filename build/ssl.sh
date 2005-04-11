	echo "Checking for OpenSSL ..."

	OSSLINC=""
	OSSLLIB=""
	for DIR in /opt/openssl* /opt/ssl* /usr/local/openssl* /usr/local/ssl* /usr/local /usr /usr/pkg /opt/csw /opt/sfw/ssl*
	do
		if test -d $DIR/include/openssl
		then
			OSSLINC=$DIR/include
		fi

		if test -f $DIR/lib/libcrypto.so
		then
			OSSLLIB=$DIR/lib
		fi
		if test -f $DIR/lib/libcrypto.a
		then
			OSSLLIB=$DIR/lib
		fi
	done

	if test "$USEROSSLINC" != ""; then
		OSSLINC="$USEROSSLINC"
	fi
	if test "$USEROSSLLIB" != ""; then
		OSSLLIB="$USEROSSLLIB"
	fi

	if test -z "$OSSLINC" -o -z "$OSSLLIB"; then
		echo "OpenSSL include- or library-files not found."
		echo "Although you can use Hobbit and bbgen without OpenSSL, you will not"
		echo "be able to run network tests of SSL-enabled services, e.g. https."
		echo "So installing OpenSSL is recommended."
		echo "OpenSSL can be found at http://www.openssl.org/"
		echo ""
		echo "If you have OpenSSL installed, use the \"--sslinclude DIR\" and \"--ssllib DIR\""
		echo "options to configure to specify where they are."
		echo ""
		sleep 3
		echo "Continuing with SSL support disabled."
	else
		# Red Hat in their wisdom ships an openssl that depends on Kerberos,
		# and then puts the Kerberos includes where they are not found automatically.
		if test "`uname -s`" = "Linux" -a -d /usr/kerberos/include
		then
			OSSLINC2="-I/usr/kerberos/include"
		else
			OSSLINC2=""
		fi

		cd build
		OS=`uname -s` $MAKE -f Makefile.test-ssl clean
		OS=`uname -s` OSSLINC="-I$OSSLINC $OSSLINC2" $MAKE -f Makefile.test-ssl test-compile
		if [ $? -eq 0 ]; then
			echo "Found OpenSSL include files in $OSSLINC"
		else
			echo "WARNING: OpenSSL include files found in $OSSLINC, but compile fails."
		fi
	
		OS=`uname -s` OSSLLIB="-L$OSSLLIB" $MAKE -f Makefile.test-ssl test-link
		if [ $? -eq 0 ]; then
			echo "Found OpenSSL libraries in $OSSLLIB"
		else
			echo "WARNING: OpenSSL library files found in $OSSLLIB, but link fails."
		fi
		OS=`uname -s` $MAKE -f Makefile.test-ssl clean
		cd ..

		OSSLINC="$OSSLINC $OSSLINC2"
	fi

