#!/bin/env sh

goblin_git='github.com/k1574/goblin'
pwd=`pwd`
export ROOT=$PWD

which goblin || go get -u $goblin_git && go install $goblin_git

echo [ mk $@ ] && mk $@

