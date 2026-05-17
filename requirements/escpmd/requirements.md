# ESC/P Markdown Filter

You are to write a separate C-program called escpmd which stands for 'ESC/P Markdown Filter'.
Using your knowledge of ESC/P support, and your knowledge of markdown, create an implementation plan
on how to write a filter. The filter should either takes input from `stdin` and pipe to `stdout` or accept one or more
files to process, with a `-o` or `--output` flag specifying the output file (otherwise sending to `stdout`).

## Refactoring

You will need to refactor the current program `escp` to place the knowledge about ESC/P codes in a separate file
so that can be used in both programs. Don't duplicate effort - refactor where you can.

## Additional Requirement - UTF-8 Handling

Although we can use an external utility, can you investigate if there is a library that allows us to build
in UTF-8 conversion - the output character set should be fixed initially to ISO-8559-1.

## Additional Requirement - smily handling
szx
If there is an establish standard for smily shortcuts such as :smile: and :wink: implement an option that converts
these to their ASCII equivalents.

## Requirement modification - link handling

For links, remove the markdown braces and write the link text in underline, don't show the URL target.