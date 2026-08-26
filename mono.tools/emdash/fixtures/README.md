# em-dash-check fixtures

Three files, and between them they are the reason `just em-dash-check` cannot go
green by scanning nothing.

- `planted.md` holds one U+2014. The recipe scans it on every run and fails if it
  is not reported, so a scanner that has stopped matching is caught by the check
  itself rather than by the next person to write an em dash.
- `expect` names the file and the line the em dash is on. Move the line and this
  is what tells you to update it.
- `clean.md` holds every character an em dash is confused with: a hyphen, a
  double hyphen, an en dash in a range and a minus sign. Reporting any of them is
  a failure, because a numeric range is not a style violation.

The directory is excluded from the tree scan for the obvious reason.
