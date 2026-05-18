# AAC Direct License Review

Date: 2026-05-18

This is an engineering release note, not legal advice. It records the licensing
and patent issues that must be considered before distributing AAC Direct builds.

## Scope

- StreaMu now links the Helix AAC decoder core into the 3DS app binary.
- The server-side AAC Direct path transmuxes YouTube MP4/fMP4 audio to ADTS; it
  does not decode AAC.
- MP3 Proxy remains available as fallback/debug, but AAC Direct is the default
  playback path.

## Sources Checked

- Local license: `third_party/helix-aac/RPSL.txt`
- Local import note: `third_party/helix-aac/README.md`
- Upstream import source: <https://github.com/pschatzmann/arduino-libhelix>
- RPSL reference: <https://opensource.org/license/RPSL-1.0>
- SPDX RPSL text: <https://spdx.github.io/license-list-data/RPSL-1.0.html>
- Via LA AAC licensing program: <https://www.via-la.com/licensing-programs/aac/>

## Findings

### RPSL applies to the bundled Helix AAC code

The upstream Arduino libhelix README states that the decoder code is from the
Helix project and licensed under RealNetworks' RPSL. The local import keeps
`RPSL.txt` in `third_party/helix-aac/`.

The RPSL conditions matter for binary releases:

- Preserve copyright, proprietary notices, disclaimers, and license references
  in copies of the original code.
- Include a copy of the RPSL with distributed source and documentation.
- Make externally deployed modifications to covered code publicly available
  under the RPSL.
- If distributing object/executable form only, include a notice that source code
  for the covered code is available under the RPSL and explain where to obtain
  it.
- Include the object code notice from Exhibit A where copyright notices are
  placed:

```text
Helix DNA Client technology included. Copyright (c) RealNetworks, Inc.,
1995-2002. All rights reserved.
```

RPSL also has strong derivative-work language. The current project license can
remain MIT for independently developed StreaMu code, but releases must not imply
that the bundled Helix AAC code is MIT.

### AAC patent licensing is separate from RPSL

RPSL does not clear third-party codec patents. RPSL section 2.2 explicitly
places responsibility for other required intellectual-property rights on the
distributor/user.

Via LA's AAC program states that an AAC patent license is needed by
manufacturers or developers of end-user encoder and/or decoder products, and
that the program covers AAC-LC among other AAC technologies. It also states
that there are no patent license fees for AAC bitstream distribution itself;
fees relate to encoder/decoder products.

For StreaMu, the potentially relevant product is the 3DS app binary containing
an AAC decoder. The server component only transmuxes and should be treated
separately from the decoder patent question.

## Release Requirements

Before distributing an AAC Direct build:

- Keep `third_party/helix-aac/RPSL.txt` in the source tree.
- Include `THIRD_PARTY_LICENSES.md` in source archives and release artifacts.
- Make the exact source corresponding to the distributed binary available.
- Include the Helix object code notice in release notes or another visible
  third-party notice location.
- Do not describe the whole binary as simply MIT licensed. Say that StreaMu
  project code is MIT, with bundled third-party components under their own
  licenses.
- Keep AAC patent status as "not cleared by RPSL".

## Recommendation

RPSL notice/source obligations can be handled in this repository with the
current source-public release model plus strengthened third-party notices.

AAC patent clearance remains unresolved for public binary distribution of the
3DS app. For a non-commercial homebrew release, the practical risk may be
different from a commercial product, but that is a release-owner/legal judgment,
not something this review can clear.

Recommended release stance:

- OK to continue engineering and testing AAC Direct.
- Do not claim AAC patent clearance.
- Before broad public binary release, decide whether to accept the AAC patent
  uncertainty, seek legal advice, or investigate a distribution model that
  avoids shipping an AAC decoder binary.
