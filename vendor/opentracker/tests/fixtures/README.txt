tl_project_template.mt: the LATEST_PROJECT_TEMPLATE embedded in Polyend
tracker-lib src/data/project.ts (base64-decoded, 2324 bytes). MIT licensed,
(c) 2026 Polyend; see ../../third_party/LICENSES/tracker-lib.txt.

mini_pattern_golden.hex, mini/<project>/: Tracker Mini (docs/specs/21) fixtures
written by gen_mini_fixtures.py, an implementation independent of the C++
codecs using the tracker-lib PatternConstants: a 0x3030 16-track pattern
golden and two Mini projects (project.mt built on tl_project_template.mt with
synth-slot records, names and mutes of tracks 13-16; 0x3030 patterns with
steps on instruments 64-66). Regenerate: python3 -I gen_mini_fixtures.py .
