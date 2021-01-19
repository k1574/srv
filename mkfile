<mkconfig

MKSHELL = rc

DIR = http/d \
	gopher/d \
	md/2/htm \
	pp \

dep-% :QV: 
	cat $DIRS^/dep/$stem | sed '/^$/d' | goblin uniq -U

& :QV:
	pwd = `{pwd}
	for(d in $DIR){
		echo [ cd $d ';' mk $target ]
		{ builtin cd $d ; mk $MKFLAGS $stem ; builtin cd $pwd }
	}

