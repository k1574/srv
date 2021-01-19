<mkconfig
SRC = spp.c main.c
OBJ = ${SRC:%.c=%.o}
HDR = `{ls *.h}
TGT = $NAME
all :Q: $TGT 
	echo -n
$TGT : $OBJ
	$LD -o $target $LDFLAGS $prereq
CFLAGS = $CPPFLAGS
%.o :V: %.c
	$CC -c -o $target $CFLAGS $prereq
%.c :N: $HDR
%.h :N:
clean :V:
	rm -rf $TGT *.o
install :V: $TGT
	mkdir -p $EXE $MAN/1
	cp -f $TGT $EXE/
	chmod 0755 $EXE/$TGT
	cp man $MAN/1/$TGT
	chmod 0644 $MAN/1/$TGT
uninstall :V:
	rm -f $EXE/$TGT $MAN/1/$NAME
