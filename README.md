mpd-dsd-018
===========

Version of stable MPD 0.18 for fasttracking DSD playback development

Development train:

develop in mpd-dsd-018 -> when a feature is ready, propose as patch to mpd-devel 

[history]
05-Nov-13 Patch To enable DSD128 in DSF decoder sent to MPD devel list

07-Nov-13 MPD 0.18.2 released with DSD128 patch for DSF included

09-Nov-13 Fixed DSD DSDIFF decoder, patch sent to MPD devel list

10-Nov-13 DSDIFF fix is added to MPD 0.18 git

11-Nov-13 Added temporary workaround for hang at the end of certain songs (DSF
and DSDIFF)

13-Nov-13 MPD 0.18.4 released with DSDIFF fix included

02-Mar-14 Updated to MPD 0.18.9. Currently no (real) differences with upstream
	  MPD.

08-Mar-14 Added seek support for DSDIFF and DSF DSD decoders

04-Apr-14 Report bitrate for DSDIFF and DSF DSD decoders

26-May-14 Update to MPD 0.18.11

20-Jun-14 Prepare for DSD native output support

04-Jul-14 Dsf: allow up to DSD512, enable DSD rates based on Fs=48kHz

14-Jul-14 Update native DSD support

13-Aug-14 Update to MPD 0.18.12
10-Oct-14
- Update to MPD 0.18.13
- Update to MPD 0.18.14
- Update to MPD 0.18.15
- Update to MPD 0.18.16

20-Oct-14 Implement native DSD support for XMOS based USB DACs.
	See [xmos-native-dsd] (https://github.com/lintweaker/xmos-native-dsd) for more
	information.

09-Nov-14
- Update to MPD 0.18.17

21-Nov-14
- Switch to DSD_U32_BE for XMOS/Denon/Marantz native DSD output

29-Nov-14
- Update to MPD 0.18.18
- Update to MPD 0.18.19

13-Nov-14
- Update to MPD 0.18.20

28-Jan-15
- Update to MPD 0.18.21
- Update to MPD 0.18.22
- Allow larger ID3 tags for DSD


