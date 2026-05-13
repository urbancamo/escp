#!/bin/zsh
{
    escp --init --cpi 10
    fold -s -w 132 "$1" | iconv -f UTF-8 -t ASCII//TRANSLIT//IGNORE | pr -w 132 -F
    escp --ff
  } | lp -d PP404LP
