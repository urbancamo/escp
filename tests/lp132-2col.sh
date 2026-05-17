#!/bin/zsh
  {
      escp --init --cpi 12 --condensed --quality draft
      fold -s -w 132 "$1" \
        | iconv -f UTF-8 -t ASCII//TRANSLIT//IGNORE \
        | pr -w 264 -F -2 \
        | awk 'BEGIN{RS="\f"; ORS=""} NR>1{printf "\f"} {sub(/^\n\n/, ""); print}' \
        | expand
      escp --ff --init
  }  | lp -d PP404LP
