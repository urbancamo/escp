# Table Requirements

Currently tables are poorly generated. We need a separate
method to deal with tables.

## Requirements

 - tables should always fit horizontally.
 - table columns should line up.
 - table headings should be displayed in bold

We need a heuristic that measures the width of the table
and fits it into the available page width. This will 
depend on the default font size. If necessary then before
the table is emitted we can reduce the font size so it is
likely to fit.

Create an implementation plan that takes into account these
features. Highlight any questions you may have.