mpd-dsd-018
===========

Version of stable MPD 0.18 for fasttracking DSD playback development

Development train:

develop in mpd-dsd-018 -> when a feature is ready, propose as patch to mpd-devel 

[history]
05-Nov-13 Patch To enable DSD128 in DSF decoder sent to MPD devel list
07-Nov-13 MPD 0.18.2 released with DSD128 patch for DSF included
09-Nov-13 Fixed DSD DSDIFF decoder, patch sent to MPD devel list

NB ** DSDIFF playback is currently broken in MPD 0.18 to 0.18.3. Somewhere in the
transition from 0.17 to 0.18 (and c code to c++) it broke. DFF files cannot be
played back.

