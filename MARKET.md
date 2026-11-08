# Market analysis

A working document on where phig sits against existing photo management
software, what the field's recurring weaknesses are, and which of phig's
properties are actually differentiated. Findings are dated; add to it rather
than rewriting it, so reversals stay visible.

Nothing here is a commitment. Positioning claims that depend on unbuilt
features say so.

## Status

- **Last updated:** 2026-08-08 — the newest section is a recorded position with
  no new sources behind it; everything dated 2026-08-05 and earlier is research.
- **Sources so far:** three buying-guide roundups, targeted web searches, and six
  forum/Reddit threads read in full. No user interviews, no download or
  install-funnel data, no measurement of any competitor.
- **Confidence:** moderate on what users say they want (six independent threads
  agree). Low on market size, willingness to pay, and anything about mobile.
- **Primary sources read directly:** six, all supplied by hand —
  DPReview "Photo Supreme vs iMatch" (prosumer/DAM), r/AskPhotography
  "local photo management that doesn't use AI" (general user leaving Google
  Photos), r/photography "how do you organize your photos" (broad/working
  photographers), r/unRAID "latest go-to photo management app" (self-hosters),
  and two posts by the author of **Lap** — r/Windows11 (685 upvotes) and
  r/macapps — a shipped competitor built to nearly this exact spec.
- **Headline conclusion after six sources:** the demand is real and repeatedly
  voiced, the requirement list is consistent across audiences, and **the position
  is already contested by a shipped product with a working UI.** phig's engine is
  deeper; its client is not competitive. See the Lap sections.
- **Known source gap:** Reddit and DPReview both block automated fetching
  (403 / Cloudflare challenge), so their threads are reachable only via
  search-result summaries or by a human saving the page. Anything attributed to
  them below is secondhand **unless marked "read directly."**
- **Biggest single finding:** a prosumer leaving Lightroom and a non-photographer
  leaving Google Photos independently asked for the same five things — local,
  one-time cost, no editor, easy to learn, organizing that beats folders. See
  "Single product for both audiences?". Lap's pitch is that same list, which is
  both the validation and the warning.

## The field splits into five segments

Surveying [Digital Camera World](https://www.digitalcameraworld.com/buying-guides/best-photo-organizing-software)
(updated 2026-03-30), [Tom's Guide](https://www.tomsguide.com/best-picks/best-photo-organizer-apps)
(updated 2025-06-12), and [Aiarty](https://www.aiarty.com/edit-photo/best-photo-management-and-organizing-software.htm),
the products cluster into groups that barely compete with each other. The first
three come from those roundups; segments 4 and 5 were missing from all of them and
were found through forums.

### 1. Cloud storage with an organizer attached — not addressable

Apple Photos/iCloud, Google Photos, Amazon Photos, Flickr Pro.

Tom's Guide's entire recommendation logic is "which phone do you have," and its
FAQ concedes: "if you only have a few photos... do you really need to organize
them?" This is a bundling war between Apple, Google, and Amazon. Storage
economics decide it. Not a market to enter.

Worth noting anyway: these set the baseline expectation for search quality.
Google Photos does natural-language visual search on untagged photos.

### 2. Prosumer desktop editors that organize as a side effect

Lightroom Classic, Lightroom, Capture One Pro, Zoner Photo Studio, Elements
Organizer, Adobe Bridge, ON1 Photo RAW, ACDSee Photo Studio Ultimate.

Mostly subscriptions ($11.99–19.99/mo for Adobe; Capture One dearer; Zoner
$59/yr). ON1 and ACDSee offer perpetual licenses.

**Excire Foto 2025 is the important one here.** $189 one-time, organizing only,
explicitly no editing. DCW made it "Best AI" and its reviewer called the
missing editor a virtue: "in many ways this is a blessing because the UI is laid
out simply and effectively." That is direct evidence a pure organizer can hold a
category against editors — the closest existing product to what phig is.

### 3. Local-first / self-hosted — phig's actual neighborhood

digiKam, XnView MP, Mylio Photos, Immich, PhotoPrism, **Ente**. Plus two
categories the buying guides omit entirely — see below.

**Immich is the gorilla, and it is accelerating** (added 2026-08-05).
~90k GitHub stars in late Jan 2026 → ~98k mid-April → ~107.5k mid-July, crossing
108k in July 2026. v3.0.0 shipped 2026-07-01, now developed full-time under
**FUTO** funding. Described as arguably the most popular self-hosted project of
the last two years, and the gap over PhotoPrism keeps widening. Its 1.140 release
added **background mobile sync** (OS-managed upload queue, periodic background
sync) — still beta, and it does not remove the always-on server requirement, but
it narrows the ingest gap. A well-funded, fast-moving incumbent with real mobile
apps is the main competitive risk to any self-hosted-adjacent positioning.

**Immich vs PhotoPrism, as the community frames it:** Immich has native iOS and
Android apps with auto-sync; PhotoPrism has no native mobile app (browser/PWA
only). PhotoPrism is easier to install; Immich moves fast and configs can break
between releases, so it is poor for "set and forget." Immich's face detection is
reported better across diverse skin tones. Verdict pattern: Immich to replace
Google Photos for a family; PhotoPrism if the library already lives on a NAS.

**Ente** — not in any of the three buying guides. End-to-end encrypted, open
source, self-hostable, with real apps on Android, iOS, web, and desktop.
Cryptography externally audited (Cure53, Symbolic Software, Fallible). Positioned
as cloud convenience without surveillance. Relevant because it occupies the
privacy high ground with genuine cross-platform reach — the same pitch phig would
make, executed by a funded team, though with a hosted-cloud default rather than
local-first.

**PhotoView** — minor; a folder-tree gallery with password-protected share links.
Mentioned only 42 times since March 2021 versus Immich's volume. Not a
competitive factor.

### 4. DAM-only tools — omitted by all three buying guides (added 2026-08-05)

**The most important gap in the original analysis.** None of the three roundups
mention this category, but photography forums discuss it heavily. These are
phig's closest commercial analogues: organizing, no editor.

| Product | Price | Platform | Notes |
|---|---|---|---|
| **Photo Supreme** (IDimager) | **$129 one-time**, perpetual | Win/Mac/Linux | Generic DAM, image-specialized. Praised for keywording and geotagging. Users report switching *from* iMatch for a friendlier UI. |
| **iMatch** (photools.com) | **$135 flat** perpetual (Home from $55) | **Windows only** | Deep, powerful, power-user reputation. Handles images, video, PDF, Office docs. |
| **Daminion** | — | Windows, server editions | Described as used for "light-weight DAM work"; also has a team/server product. |
| **digiKam** | Free | Win/Mac/Linux | Repeatedly named the best free alternative to Photo Supreme. |

Forum framing worth noting: participants say there are *few* DAM-only options —
"Digikam (open source), Daminion, Photo Supreme and iMatch" — and treat Photo
Supreme and iMatch as the top two. That is a short list, and phig is a candidate
for it.

Sources: [Photo Supreme vs iMatch](https://www.dpreview.com/forums/thread/4536709),
[iMatch 5 vs PhotoSupreme 4](https://www.dpreview.com/forums/thread/4256729),
[Daminion alternatives](https://www.dpreview.com/forums/thread/4637597),
[Photo Supreme pricing](https://www.idimager.com/pricing),
[iMatch pricing](https://www.capterra.com/p/126559/IMatch/pricing/).

**Implication for pricing:** the DAM-only category clusters at **$129–189
perpetual** (Photo Supreme $129, iMatch $135, Excire $189). That is a real,
consistent price band for organizing-without-editing, and it is the band phig
would enter.

## Primary sources — threads read directly

The six sections below are user voices read in full, not reviewer summaries.
Ordered as they were gathered. Each notes its own sample bias; the useful signal
is where independent audiences agree, not the vote counts within any one thread.

### DPReview: "Photo Supreme vs iMatch"

First primary source in this document. Thread opened Dec 2020, revived Nov 2024
([thread/4536709](https://www.dpreview.com/forums/thread/4536709)), read from a
locally saved copy. ~26 posts. Includes iMatch's author (Mario M. Westphal)
answering in-thread. Small sample, self-selected toward DAM enthusiasts — but
these are users' own words, not a reviewer's.

**The opening post is the most useful thing here.** An unprompted statement of
requirements from someone actively leaving Lightroom:

> "I am looking for a DAM that is easy to learn, stable and not too expensive."

Plus, separately: cull on import, and compare similar shots side by side to pick
the keeper — explicitly *without* adding another tool ("I am hoping to reduce the
tools in the workflow").

Ranked, that is: **easy to learn, stable, cheap, culling, similar-shot
comparison, tool consolidation.** Note what is absent — no mention of AI, cloud,
mobile, sharing, or face recognition. Six wants, and phig currently addresses
one and a half (cheap; near-duplicate grouping is adjacent to similar-shot
comparison but is not a compare UI).

**Recurring themes across the thread:**

- **Culling is the workflow gap everyone talks about.** Most of the thread is
  about culling tools, not DAM features. Participants use FastRawViewer, Fast
  Picture Viewer, or Photo Mechanic *alongside* their DAM. Photo Supreme's
  "Light Table" (up to 8 photos, synchronized zoom/pan, per-box histogram) and
  iMatch's Viewer are the in-DAM answers. A user's typical loop: star-rate, delete
  the 1s and 2s, then review what remains.
- **RAW histogram during culling is treated as essential** by several
  participants ("Being able to see the raw histogram is now essential").
- **Developer responsiveness is a named buying criterion**, mentioned repeatedly
  and independently for both products. So is the user forum. For a solo-developer
  tool this is part of the product.
- **Learning curve is the standing complaint about iMatch** — "incredibly
  powerful, although there is a learning curve," and its author agrees: "IMatch
  is a grow-up application with a very large feature set. Don't expect to learn
  everything in a couple of days."
- **Long tenure.** One user on Photo Supreme "since apx 2007." These buyers
  switch rarely, which cuts both ways: hard to win, then durable.
- **"Just try the trials" is contested.** One participant said only you can
  judge; the OP pushed back — a 30-day trial cannot reveal "known bugs or
  usability issues," so expert opinion is needed. Argues for visible, credible
  engineering claims over a download button.
- **2024 status check** (post #24, by `gaul`): both still viable. Photo Supreme
  is Win+Mac; **iMatch is Windows-only.** Consensus framing: iMatch slightly more
  advanced, Photo Supreme slightly more user-friendly.
- **Multi-user/remote is asked about and barely served.** iMatch supports
  database sharing; "IMatch Anywhere" is a separate paid server product that is
  **read-only** ("cannot create or update IMatch databases").

**What this says for phig:**

- **The stated top requirement is "easy to learn" — phig's weakest axis.** A CLI
  is not that, and the incomplete GTK client is not yet either. This is the
  clearest user-voiced statement of the gap identified at the end of this
  document.
- **Culling + similar-shot comparison is a real, repeatedly-voiced need that
  phig's engine is unusually well-positioned for.** Near-duplicate grouping at
  160k/1.7s plus `blur_score` is most of the hard part of "show me the near
  -identical shots and help me keep the best one." Nobody in this thread has that
  built on measured phash grouping; they compare manually on a light table. A
  duplicates-review/culling UI may be a stronger wedge than P2P sync — it is
  closer to shipped capability and answers a want users state unprompted.
- **No mobile, cloud, or sync demand appears anywhere in this thread.** Either
  the P2P thesis targets a different audience than these DAM buyers, or that
  demand has to be found elsewhere. Worth resolving before committing to five
  platforms.
- **macOS matters, Linux is unmentioned.** Photo Supreme's Win+Mac coverage is
  cited as an advantage over iMatch. phig's Linux-first position is not an
  asset to this audience.
- **Sample caveat:** one thread, ~26 posts, DAM enthusiasts on a
  camera-gear forum. Directional, not representative.

### r/AskPhotography: "local photo management software that doesn't use AI?"

Second primary source, and the **first general-user** voice in this document.
Posted ~May 2026, ~30 comments, supplied by hand. This is the
leaving-Google-Photos audience the P2P thesis targets, so it carries more weight
for that question than the DAM thread does.

**Who they are:** explicitly not a photographer. Phone photos, screenshots, and
video — "mountains" of it, "several thousands or even tens of thousands."
Leaving Google Photos over AI-training and storage-upsell distrust ("a shared
scam"). Buying external drives and learning to burn Blu-Ray. Windows 10.

**What they asked for, in their words:**

- "compatible with Windows 10 which runs **entirely locally**"
- "**happy to pay for it, preferably a one-off cost**"
- "don't need anything fancy or any complicated editing tools"
- "an **intuitive UI** that makes life easier than scrolling through thousands
  and thousands of images"
- "lets me **group and tag** things so I can either search for photos of my dog
  or hikes and **the same photo would show up cos it's tagged with both**"

**The convergence with the DAM buyer is the headline finding.** A prosumer
leaving Lightroom and a non-photographer leaving Google Photos independently
asked for the same five things: local, one-time payment, no editor, easy to
learn, and organizing that beats folders. Two audiences, one requirement list.
This is the strongest evidence so far that a single product can serve both — and
it was the open question at the top of the "Single product" section below.

**On AI — a nuanced position, not blanket rejection.** They pre-empted the
objection themselves: if the "AI" is "just a fancy programme that doesn't
communicate with anything and doesn't collect any data I'm okay with that," and
cited their dad's local face recognition as the acceptable kind. A commenter
made the same distinction about digiKam — optional, disabled by default,
locally-trained, "predating modern AI models." **The demand is not
"no AI," it is "no cloud AI, no data collection, no upsell."** phig's YuNet/SFace
setup is exactly this: local ONNX models, `phig models download` once, no
telemetry, no account. That is a marketable property, and it inverts the earlier
read that AI is only table stakes — *provenance* of the AI is the
differentiator, not capability.

**The pain point, described concretely.** Their current process:

> "clicking each one open to view it, close, ctrl-x, navigate to new folder,
> ctrl-v (repeat 1-5 times), navigate back to 'unsorted' folder"

And why folders fail them: one photo needs to be findable as *hike*, *sister*,
*dog*, and *drawing reference* at once — "in a folder system I'd need a copy in
the 'Hike 2026-04-30' folder, and a copy in the '?Drawing' folder." **This is
the argument for tags over folders, discovered independently by a
non-technical user.** phig's N-tags-per-image model answers it directly; the
one-gallery-per-image constraint does not.

They also want the tag-propagation loop their dad's software had: name enough
faces, it auto-detects new ones, puts them in a reviewable group, and moving a
tag re-files automatically. phig's `face identify` propagation is the same idea
without the review step.

**What the community actually recommended** (frequency, ~30 comments):

| Recommendation | Times | Note |
|---|---|---|
| **digiKam** | 3 | Most-recommended. Free, FOSS, cross-platform, local optional AI. "appeals to my wallet better." |
| **Photo Mechanic** (Camera Bits) | 2 | Multi-platform, fully local, "super fast." Praised for bulk culling — 1000 images in ~10 min. Subscription, "cancel when you're done." |
| **Picasa** | 3 mentions | **Discontinued, still actively used.** "I have been using Picasa for years! Still use it and love it." Named as the thing the OP was remembering. |
| Czkawka | 1 | For duplicates specifically. |
| XnView MP | 1 | File sorting, batch rename. |
| Immich (via TrueNAS) | 1 | OP's reaction: "I don't know what half of that means." |
| PhotoPrism | 1 | — |
| FastStone | 1 | Free. |
| Phototheca | 1 | New name; not previously logged. |

**Findings that change the analysis:**

1. **Picasa is the ghost in this market.** Discontinued for a decade, still the
   reference point for what people want, still in daily use. One comment:
   "Facial/Object recognition used to be local, Picasa did it great, so great
   it's what became the basis for Google Photos... PEAK ENSHITTIFICATION!" The
   unmet want is a modern Picasa — local, simple, face recognition, no cloud.
   That is a clearer product description than anything derived from the buying
   guides.
2. **Nobody suggested a P2P sync tool, and the OP deprioritized cross-device
   access unprompted** ("Viewing across devices isn't necessarily the biggest
   priority"). Their target is an external drive and Blu-Ray, not a phone. Second
   independent thread with **zero P2P demand.** The thesis now has two sources
   against it and none for it.
3. **Immich's setup cost is disqualifying for this audience, confirmed in the
   user's own words.** The one Immich recommendation (TrueNAS + Docker) drew "I
   don't know what half of that means." Aiarty's "requires technical knowledge"
   con is real and it is a hard wall, not a speed bump.
4. **Culling appears again**, from the other audience — Photo Mechanic praised
   specifically for bulk culling speed. Two of two threads independently surface
   culling. This is now the most consistently-voiced need in the file.
5. **Windows is where this audience is.** Windows 10 named in the question; the
   DAM thread cited Win+Mac. Two of two threads, no Linux. phig's platform
   priority is inverted relative to demand.
6. **An unanswered question at the end of the thread is itself a signal:**
   another user asked 21 days ago whether the OP ever settled on something —
   "Im looking for the same thing to sort my wifes 35k+ photos." No answer. The
   need recurs and goes unmet.

**Sample caveat:** one thread, ~30 comments, self-selected toward the
privacy-motivated. But it is the exact target user, and the requirement overlap
with the DAM thread is the useful signal, not the vote counts.

### r/photography: "how do you organize your photos?"

Third primary source, ~30 comments, posted ~March 2026 with replies through
~July. Broadest sample so far: working photographers, hobbyists, and
self-hosters answering an open question with no product framing. **It partly
contradicts the r/AskPhotography thread, which is the useful part.**

**Finding 1: folders won, overwhelmingly.** Roughly two-thirds described a
`YEAR/Month/Event` (or `client/date`) folder tree on disk as their primary
system. Several explicitly value it as software-independent:

> "having that folder structure is the safety valve, no matter what happens with
> my preference of software I am counting on computers being able to read JPEG in
> folder structure for a very, very long time."

> "The folder structure makes it easier to move to another tool."

**This is phig's `mv`/`cp` format-string feature described as a user
requirement** — `%Y/%m/%d/%camera` produces exactly the layout this thread
converged on, and phig leaves the files as plain readable files in it. Nobody in
the thread had a tool that both maintains the tree *and* indexes it; they run
Lightroom's import to make folders, then organize inside Lightroom.

**Finding 2: tagging is contested, not universally wanted.** The
r/AskPhotography thread produced a clean argument for tags over folders. This
thread splits:

- *Pro-tag:* "I can simply search 'lighthouse' and 'ohio' and it'll come up
  quickly" — vs. hunting through a lighthouses/Ohio/Cleveland/Fall-2024 folder.
  One user keywords and geotags **1.5 million** photos in Lightroom Classic and
  finds anything "within a few seconds."
- *Anti-tag:* "I don't have the patience to keyword and tag, so I've come to rely
  on services with AI for that." And: "I've also abandoned any attempts at
  getting clever with tags and metadata... A simple naming convention is 90% of
  what I need."

**Implication:** manual tagging has a real adoption cost, and a segment will not
pay it. The people who abandoned tagging went to AI auto-tagging or gave up. A
product that *requires* manual tagging to be useful loses them. phig's automatic
axes (date cascade, phash grouping, faces, `blur_score`) matter more than its
tag surface for this segment.

**Finding 3: Lightroom Classic scales far past what was assumed.** 1.5M photos
with extensive keywording, "almost any photo within a few seconds." Another at
~500k on Windows 11 with "no discernible performance issue." Contested claim that
Windows degrades with large libraries while macOS stays smooth — reported
secondhand within the thread, and directly contradicted by the 500k Windows user.
**Do not assume Lightroom has a scale weakness phig can exploit.** phig's 30k
reference collection is two orders of magnitude below what LrC users run
routinely.

**Finding 4: Immich shows up as an *addition* to a folder workflow, not a
replacement.** Two detailed setups: RAW/JPEG in a hand-maintained NAS tree,
Immich pointed at it as an **external library** with RAW and unedited folders
excluded from indexing, giving "a Google Photos like interface" while "my files
never touch the cloud." Another Lightroom user is "testing Immich" for iPhone
shots and considering extending it to DSLR work.

This is the most competitively significant item in the thread. Immich's
external-library mode is doing the job phig is designed for — index files in
place, leave them alone, give a browsable timeline — and it arrives with mobile
apps and face recognition. **It also means the always-on-server objection is
priced in by this audience**, who already run a NAS.

**Finding 5: no Picasa nostalgia, no anti-AI framing.** Where the
r/AskPhotography thread was organized around distrust of AI, this thread mostly
does not raise it. One user calls the big tools "very intrusive and they steal
your photos for AI use," and one says "upload to Google photos. let AI take the
wheel." **The anti-AI position is a real segment, not the market's default.**

**Finding 6: culling barely appears** — a striking absence given both prior
threads surfaced it. One mention of a "dump and run" workflow with lost track of
image sets, and one about generating contact-sheet proofs. Culling is a
prosumer-with-burst-shooting need (DPReview) and a bulk-import need
(r/AskPhotography), not a universal one. **Downgrade slightly** from "most
consistently voiced need."

**Finding 7: a wave of new small tools, several by their own authors in-thread.**
Sortio (plain-text rules → folder moves), NameQuick (descriptive filenames from
image content), Foldora AI, AI File Sorter, **Rollfilm** (open-source desktop
photo manager, hobby project, rollfilm.org), discoverfile, FastStone (two
mentions across threads). Plus Photos.app on macOS praised for finding
duplicates, faces, tags, geolocation — with the complaint that the external disk
can never be ejected.

Two readings, both worth holding: the problem is unsolved enough that people keep
building tools, **and** the space is crowded with hobby projects, so "I built a
local photo organizer" is not itself differentiating. Note also that the
AI-filename-suggestion tools are all positioned as *pre-processors* alongside
Lightroom/digiKam/Immich, not replacements.

**Finding 8: the honest summary of the whole market, from one commenter:**

> "if you are organized and disciplined there are a lot of options. If you are
> not, and love to take photos, you wind up with a mess. And, the feeling that it
> will take months to untangle, which you procrastinate."

The unmet need is not a better catalog for disciplined people — they are served.
It is untangling an existing mess without months of work. phig's automatic axes
plus dedup target exactly that, and the "months to untangle, so I procrastinate"
framing is the strongest description of the buying trigger in any of the three
threads.

### r/unRAID: "Latest 'go to' Photo management app??"

Fourth primary source, ~80 comments, posted ~Jan 2026. **The most consequential
thread in this document, and the least favorable to phig.** Self-hosters with a
NAS — the audience most likely to adopt a local-first photo tool, and the one
where phig would expect to compete best.

**Finding 1: Immich has won this segment outright.** Not "is popular" — has won.

- Top comment, ~60 upvotes: "Immich is the top one these days."
- OP, after reading replies: "I love that fact that immich is apparently just the
  go-to one.. I guess I just need to do it and not over think it!"
- A commenter explicitly asked "what other app OTHER THAN immich are people
  using?" and got two answers: Nextcloud ("not as good as Immich") and
  Nextcloud Memories (sharing is "impossible or very limited," "much better" in
  Immich). That is the whole field of alternatives this audience could name.
- Every other product mention was a drive-by with 1 upvote (ViXC, Siftr).

Roughly a dozen separate participants recommended Immich. **Zero recommended
anything else as a primary tool.** For the segment phig is architecturally
closest to, the default is settled, and it is not a close race.

**Finding 2: "index files in place, don't touch them" is the dominant requested
pattern — and Immich already does it.** This is the finding that most directly
challenges phig's positioning. The single most-endorsed configuration in the
thread:

> "Immich with an external library. EZ-PZ. I run my photo folder on my array and
> give Immich read only access to it."

> "You don't have to touch, copy, move do ANYTHING with your existing photos this
> way."

The OP's concern was exactly phig's pitch — "I'm really hoping to leave my photos
intact in my array... it having a database and moving files, etc... Just weirds me
out" — and Immich's external library answered it. Scan-in-place is listed above
under "Strong — rare in the field and already built." **Against Immich it is
parity, not advantage.** It remains an advantage against Lightroom and Excire.

**Finding 3: Immich's own storage templates cover phig's organize feature.**
Participants use [storage templates](https://docs.immich.app/administration/storage-template/)
to define the folder structure Immich writes into, and
[custom locations](https://docs.immich.app/guides/custom-locations) to split
thumbnails onto SSD and originals onto the array. The OP adopted both. That is
functional overlap with phig's `%Y/%m/%d` format strings *and* its thumbnail
cache design.

**Finding 4: PhotoSync appears in the wild, paired with Immich.** Two users run
PhotoSync (or FolderSync) for phone ingest and Immich purely as a browser:

> "I use an app called PhotoSync to backup my pictures and immich as my photo
> browser. If immich take a sh** no big deal spin up a fresh instance point to the
> external library and you are back in there in no time."

This confirms the P2P correction empirically. The serverless-ingest job is being
done today by a cheap dedicated app, and the organizer sits behind it. **The
assembled stack — PhotoSync + Immich external library — is close to the product
this analysis was proposing to build**, minus the single-binary integration.

**Finding 5: the real Immich complaints, which are the actual openings.** Not
setup difficulty — this audience runs Docker without complaint. What they raise:

- **Database fragility is the top fear.** "Are we saying that there's no way to
  retrieve your photos if Immich gets corrupted?" One user chose external
  libraries specifically as insurance: "if immich takes a sh** it's nothing to
  worry about start fresh point to the external library." Another cites a hard
  limitation: "The fact that you can't rescan internal files and have to reimport
  them was a deal breaker for me."
- **Nextcloud comparison is unflattering to Immich's transparency:** "with
  Nextcloud I can browse in to the folder share and see all my photos, however I
  can't do the same with Immich."
- **Sharing an external library is not supported** — "I can't directly share the
  external library, I can however add it to an album and share that."
- **Config sprawl.** A long sub-thread on mover settings, cache vs array, and
  which of `/data`, `thumbs`, `profile`, `encoded-video` to relocate. The OP:
  "I must have screwed something up in the docker compose then, I followed the
  guide... it puts EVERYTHING there... what did I miss?" And the docs are
  fragmented — "I just wish that guide was linked from the original unraid compose
  guide."
- **Docs lag the platform** — the official unRAID guide targets a pre-7.0 version.

**What this means for phig — read pessimistically, because that is what the
evidence supports:**

1. **Do not target the self-hosted NAS segment as a primary market.** Immich owns
   it, is accelerating (~108k stars, FUTO-funded), and already does scan-in-place,
   storage templates, and thumbnail-location control. phig has no mobile app, no
   web UI, no sharing, and an incomplete desktop client. Competing here means
   losing on every axis this audience named.
2. **The transparency/recoverability complaint is the one genuine opening**, and
   it is exactly what phig is already built around: files stay where they are,
   names unchanged, the DB is a rebuildable index rather than a container, plus an
   append-only activity log with restore snapshots. "If the database dies, nothing
   is lost, and you can re-derive it" is a promise Immich users are actively
   worrying about. **But it needs `phig log`/`phig undo` to be shipped and a story
   for DB rebuild** — currently unverifiable, same problem as the data-safety
   pitch.
3. **The rescan complaint is a concrete, small feature win** — "can't rescan
   internal files and have to reimport them" is a deal breaker for a real user, and
   phig's mtime-skip rescan does exactly this. Worth noting as a comparison point.
4. **This audience will not pay $129.** Everything recommended was free and
   self-hosted. The $129–189 perpetual band identified from the DAM category does
   not transfer here. **The two audiences differ on price even though they agree on
   requirements** — a real complication for the single-product thesis.

**Sample caveat:** r/unRAID self-selects hard for people who already own a server,
which is why setup difficulty does not appear as a complaint here but does in
r/AskPhotography. Both are true of their own audiences.

### r/Windows11: "Lap" — a competitor built to this exact spec

Fifth primary source, ~150 comments, **685 upvotes**, posted ~April 2026.
[github.com/julyx10/lap](https://github.com/julyx10/lap).

**This is the most important thread in the file.** Someone independently built
close to the product this analysis has been converging on, shipped it, and got a
large, enthusiastic response. Its pitch, in its author's words:

> "Handles very large libraries smoothly (100K+ files) · Works directly with
> local folders — no cloud sync, no forced import · Clean, modern desktop UI ·
> Local AI features: search your photos with natural language, plus face
> recognition — **all on-device, no data sent anywhere**"

Compare that to the requirement list assembled from the other four threads:
local, no forced import, scales to 100k, easy/modern UI, local-only AI. It is the
same list. **The convergence is validation of the thesis and simultaneously the
strongest competitive warning in this document.**

**Their origin story is also the market's story:** "I used Picasa for years until
Google discontinued it. Switched to FastStone after that — it's fast and powerful,
but the UI feels pretty dated. So about a year ago I started building my own."
That is the third independent Picasa reference across these threads.

**Reception — what the market rewards, evidenced:**

- 685 upvotes and ~150 comments; "hundreds of people have downloaded it."
- "I searched for years this kind of program. Holy shit! For anyone with 10.000's
  of photos this might be it."
- "This is exactly what I was looking for past 10 years."
- One user in Spanish: it made them "a new woman" — no longer hunting frantically
  through thousands of folders.
- Repeated framing as the successor to something loved: "like Irfanview on
  steroids," "modernized DigiKam," "strong ThumbsPlus vibes," "Picasa vibes."

**The demand is real and it is unmet.** Five threads now agree.

**Finding: local-only AI is the headline feature, and it is what users respond
to.** Confirms the reassessment above. "all on-device, no data sent anywhere" is
in the author's own summary, and commenters single it out. One: "For me the most
important feature would be the AI search... I'm surprised there's no equivalent on
Desktop." Another asked whether it integrates iCloud; the author declined,
citing local-first. **Nobody objected to AI per se — they objected to cloud AI.**

**Finding: Lap's weaknesses are precisely phig's engineered strengths.** This is
the actionable part. From the thread, mostly from the author's own candid answers:

| Area | Lap's state (author's words) | phig |
|---|---|---|
| **Metadata** | "still evolving," EXIF only; XMP/IPTC not written. Tags "stored internally (in Lap's own database)," don't reach file properties. Called "one of the top priorities." | Full EXIF→JSON, sidecar-aware (XMP + Takeout), date cascade. Also DB-internal tags — same limitation. |
| **Duplicates** | *(thread claim — corrected by source audit below: BLAKE3 + size prefilter, and HNSW over CLIP embeddings for similarity)* | Banded phash index, 160k/1.7s, measured. **Advantage is completeness + video, not existence.** |
| **Face recognition** | "only integrated a basic model so far; it's still very early-stage." Beta. | YuNet+SFace, KNN via sqlite-vec, propagation. Also lacks clustering/merge/split. |
| **RAW** | *(corrected below: real LibRaw integration, and `group_raw_jpeg_pairs` is a shipped setting)* | 36 formats, 3-tier pixel source with full-decode fallback. **Narrower advantage than first assessed.** |
| **Indexing UX** | 20k files = "10–20 minutes," and **"Before scanning is completed, subfolders cannot be accessed."** A user hit exactly this. | mtime-skip rescan; but ~13 thumbs/sec is the same class of cost. |
| **Robustness** | A user deleted ~100 files externally and "now, when I start Lap it hangs and nothing helps." Author's fix: delete the config and DB. | Scan reconciles stale rows, re-link by sha256, restore snapshots, WAL + flock. **A shipped-quality bug in an active project — not an architectural gap. Do not build strategy on it.** |
| **Video** | Not a focus; face detection on video requested, absent. | Scans 14 video formats, keyframe phash, unified index. |

**Finding: the feature requests are a free roadmap for this product category.**
Most-requested, in rough order: portable/no-install version with a configurable
DB location (multiple askers — including one wanting the DB synced across
machines via Syncthing); write tags back to standard XMP/IPTC so other tools see
them; geotag editing; sort by created/modified date; face-match threshold slider
plus **merge/split face groups** ("manual corrections are inevitable");
background/incremental indexing so the UI stays responsive; "show me all photos
of this person" mode; bulk copy/move shortcuts; more formats (AVIF animation,
PSD, DDS, JXL, JXR); an Immich desktop client.

Two of these are worth flagging for phig specifically:

1. **"Write tags back to XMP/IPTC" is asked for repeatedly and is a genuine
   product-design question phig has not faced.** Lap stores tags in its own DB,
   as phig does. Users want them in the files, so their data survives the tool.
   That is the same trust argument that makes scan-in-place attractive, applied to
   metadata. Currently phig's tags are only in `phig.db`.
2. **Face clustering with merge/split is requested here too** — third thread to
   ask for it. It is now the most-requested capability phig lacks.

**Finding: "Add Immich support and I'll pay you good money for this!"** A desktop
Immich client is a want in the wild. The author declined, staying local-first.
Given r/unRAID's finding that Immich owns self-hosting, "good desktop client for
an Immich library" is a real, unserved niche — though it would make phig an
accessory to a competitor rather than a product.

**Finding: implementation choices are visible and judged.** Tauri/Rust, and
commenters noticed — "I really hope... this is not an electron app," "Tauri is web
ui, not desktop," and one asking for WinUI for native feel. Also: "Thanks ChatGPT
for the post (and probably the code too)" (downvoted). A C++/GTK native
implementation is defensible against this critique, and native-ness has a
constituency. Also asked: Windows-on-ARM and NPU support, which the author
addressed (ARM yes; NPU unnecessary for their models).

**What this changes:**

- **The market is validated. The position is contested.** phig is no longer
  looking at a gap; it is looking at a race with at least one shipped entrant
  that has a working modern UI, 685 upvotes of goodwill, and an active author
  shipping releases roughly monthly.
- **phig's advantage is engine depth; Lap's is that it exists and has a UI.**
  Every phig advantage in the table above is invisible without a usable client —
  which is the same conclusion as the "gap that decides this" section, now with a
  named competitor attached.
- **Windows is where this audience is** (third thread confirming). Lap ships
  Windows-first, with Linux working and reportedly faster.
- **Free and open-source is the norm here**, undercutting the $129–189 DAM band
  further. Lap is free, FOSS, and one commenter volunteered to donate. The
  segments that agree on requirements still disagree on price.

### r/macapps: Lap again, macOS audience

Sixth primary source, ~50 comments, 24 upvotes, ~March 2026 — the same author
posting Lap to a macOS audience a month *before* the r/Windows11 post (685
upvotes). Useful as a cross-check on the same product with different users, and
as a record of what Lap looked like earlier.

**Finding 1: RAW absence was the first and loudest objection.** Top comment:
"No RAW support? I was excited at first, but **no RAW support kinda ruins the
whole idea**." Two others raised it independently ("RAW files support is needed
nowadays"). The author shipped it within about a month (v0.1.12).

**This is the clearest market validation of phig's RAW work in the file.** RAW
support is not a niche prosumer nicety — its absence was called disqualifying by
a general-audience commenter. phig's 36 formats and three-tier LibRaw pixel
source go well beyond what Lap shipped in response (preview support for 20+
types). **Corrected by the source audit: Lap ships `group_raw_jpeg_pairs`.** phig's
advantage narrows to the three-tier pixel source and format breadth.

**Finding 2: "does it work with my existing folder tree, or do I have to import?"
is the first question users ask.** Asked here explicitly, with named references —
works-in-place like DEVONthink/FlowVision, versus import-and-duplicate like Apple
Photos/Eagle: "I prefer the former." The author's answer is close to phig's own
design statement:

> "It works directly with your existing folders — no import or duplication
> required... your photos always stay in their original folders... The quick scan
> only indexes files that changed since the last scan."

That is phig's mtime-skip rescan, described as a selling point. Fourth thread to
surface scan-in-place as a requirement.

**Finding 3: star ratings + filter is the culling workflow users describe, and
phig has no ratings at all.** The exchange is specific:

> "can I use the 1–5 keys to rate 1–5 and then filter by star rating? This would
> be useful to quickly preview and go through batches of photos, rate, and cull
> 1–3 stars where I only want to keep 4-5 stars."

Lap: yes, Cmd+1–5, plus a split-view compare mode for culling. The
r/AskPhotography thread described the same loop (star-rate, delete the 1s and 2s).
**phig has tags, galleries, and favorites but no rating scale** — and ratings are
the specific mechanism users name for culling. A binary favorite does not
substitute for a 1–5 sort-and-threshold workflow. This is a small, concrete gap
with repeated demand behind it.

**Finding 4: "tags should travel with the file" — asked again, more sharply.** A
professional's version of the r/Windows11 request:

> "I'd love the option to switch over to using MacOS native Finder tags... rather
> than creating and applying tags that only exist in a Lap database. That way the
> tags (things like 'model released,' 'cleared for publication') **travel with the
> file** if I send it to a contractor or colleague."

Also requested: XMP/IPTC metadata *search*, and prioritizing existing GPS
metadata over recomputed reverse-geocoding. **Now three threads asking for
metadata that outlives the tool.** phig's tags live only in `phig.db`, same as
Lap's. For a project whose stated principle is that the database is a rebuildable
index and files are the durable artifact, tags-only-in-DB is inconsistent with
its own philosophy. Worth a plan.

**Finding 5: Apple Photos is a black box, and that is an opening on macOS.** The
author on why Lap can't read an Apple Photos library: "essentially a black box
with a fairly complex internal structure." Meanwhile r/photography had a Photos
user complaining they can never eject the external disk. Apple Photos users are
locked in and some want out; a folder-first tool is the exit. Note phig cannot
read that library either.

**Finding 6: Apple Photos' performance ceiling is lower than assumed.** "100k+
photos and still responsive? That's the part where most apps fall apart. **Apple
Photos gets sluggish way before that.**" And an M4 user: Lap thumbnailed 700MB in
~15 seconds, with minor redraw artifacts scrolling 1500 images fast. Contrast
phig's GTK M0: ~95% of frames over 16ms warm, worst frame ~1.3s miss-heavy. **A
Tauri/web-UI competitor is currently delivering better perceived scroll
performance than phig's native GTK client** — which undercuts the implicit
assumption that native C++ wins on responsiveness. Lap's thumbnailing throughput
also appears well above phig's ~13/sec, though on different hardware and file
sizes, so not a like-for-like comparison.

**Finding 7: NAS-over-SMB is a real use case and a boundary.** A user with a
Google Photos export on a slow Synology wanted background thumbnailing into a
local DB — phig's exact design. Lap requires the NAS mounted as a filesystem path;
raw `smb://` is unsupported. Worth knowing phig has the same constraint, and that
"thumbnails cached locally, originals on slow remote storage" is a wanted
configuration.

**Finding 8: editing requests arrive immediately and should be resisted.** One
commenter asked for exposure, shadows, highlights, white balance, tint, noise
reduction, sharpening, 2×/4× upscaling, object removal, and custom presets. The
author held the line: Lap manages libraries and offers basic adjustments, "not
meant to replace professional photo editing software." Two of the buying guides
plus Excire's reception say the no-editor position is correct; expect steady
pressure against it anyway.

**Also noted:** the author labels AI involvement in their own post ("[AI
Disclaimer] Code completion"), and a commenter thanked them "for open sourcing
this in the age of ai slop." Provenance signalling matters to this audience —
consistent with the local-AI finding.

## Lap: source-code audit (2026-08-05)

Cloned and read `github.com/julyx10/lap` rather than trusting thread comments.
**This corrects several claims made above from those comments — Lap is
substantially more built than its Reddit threads suggest.** ~28,000 lines of Rust
across 27 files, plus 59 Vue components. Tauri, not Electron. **GPL-3.0-or-later.**
Latest commit 2026-08-05 — active today.

### Corrections to what this document said earlier

| Earlier claim (from threads) | Reality in source |
|---|---|
| "Exact duplicates via hash+size only" | `t_dedup.rs` (912 lines): **BLAKE3** content hashing, size-prefiltered (only hashes files whose size collides), mtime-cached so rescans skip unchanged files. Excludes Live Photo companion videos. Well built. |
| "Similar images = manual cleanup" | `t_similar.rs` (500 lines): **HNSW approximate nearest-neighbour** (`hnsw_rs`) over CLIP-style embeddings, cosine ≥ 0.93, top-K 64, groups capped at 64 for reviewability. Not manual at all. |
| "Face recognition very early-stage" | `t_face.rs` (907) + **`t_cluster.rs` (301) — it has actual face clustering**, plus non-maximum suppression, and `delete_person`. |
| "RAW preview for 20+ types" | `t_libraw.rs` (595 lines) — real LibRaw integration with embedded-preview extraction. And **`group_raw_jpeg_pairs` is a user-facing setting**: it groups RAW+JPEG as one item and keeps both on copy. |
| "No metadata writeback" | Uses `little_exif` to **write** EXIF, and `t_apple_sidecar.rs` (624 lines) handles Apple AAE/XMP sidecars, including rename/rollback. Still no full XMP/IPTC write, matching the author's own statement. |
| Implied weak data safety | Trash vs. permanent delete as separate paths, `FileConflictPolicy` with Replace, conflict pre-checks, and `rollback_rename_changes` / `rollback_copied_transfers`. |

**The "Lap hangs after external deletes" report stands** as a shipped-quality
issue, but it is a bug in an actively maintained project, not an architectural
gap. Do not build a strategy on it.

### The genuinely different design choice: embeddings vs. perceptual hash

Lap's similarity is **CLIP embeddings + HNSW**. phig's is **64-bit DCT pHash +
banded index**. This is the sharpest technical contrast, and each wins different
cases:

- **CLIP/HNSW finds semantically similar images** — two different photos of the
  same subject. It also powers natural-language search from the same vectors, which
  is why Lap gets "search your photos with text" for free. Approximate: HNSW can
  miss true neighbours, and cosine ≥ 0.93 is a tuned heuristic.
- **pHash/banded finds visually near-identical images** — the same shot resaved,
  recompressed, resized, or burst-adjacent. phig's banded index is **complete by
  the pigeonhole principle** (no missed pairs below threshold) with exact Hamming
  confirmation, plus measured fallbacks. It cannot do semantic search.

**For culling — the need voiced in three threads — pHash is the better primitive**
and completeness is a real advantage: "did not miss any duplicate" is provable for
phig and not for HNSW. For "find my photos of a beach," CLIP wins and phig has
nothing.

Lap also gates similarity/dedup to `file_type in (1,3)`, so **video is largely
outside its similarity index.** phig hashes video keyframes into the same pHash
space as images.

### What phig still has that Lap does not

Verified against source, not inferred:

1. **A C ABI (`libphig`) and a CLI.** Lap has no CLI, no headless mode, no
   library target — `grep` for `clap`/`std::env::args` finds nothing, and
   `Cargo.toml` declares no `crate-type`. It is a GUI application only. **phig is
   scriptable and embeddable; Lap is not.** For anyone automating a photo pipeline
   or building another client, this is the whole ballgame.
2. **Completeness guarantees on near-duplicate detection**, with published
   measurements (160k/1.7s, a rejected BK-tree, two calibrated fallbacks). Lap's
   HNSW is approximate and its thresholds are unmeasured constants.
3. **The activity log with per-entry restore snapshots.** Lap has targeted
   rollback within an in-flight operation; phig has a persistent append-only record
   of every mv/cp/rm across sessions. Still needs `phig log`/`phig undo` to be
   user-visible — this remains the single highest-leverage unshipped feature.
4. **Video as a first-class citizen** in hashing, dedup, and search.
5. **The writer lock** (`flock` on `<db>.lock`) so concurrent processes serialize.
   Moot for a single GUI; necessary the moment a CLI and GUI coexist.
6. **Three-tier RAW pixel source** (sidecar JPEG → embedded preview → full
   LibRaw decode) validated on 177 real CR2s where 62 fail through OpenCV. Lap
   uses LibRaw previews; unclear it has the full-decode fallback.
7. **Measured everything.** phig's performance claims are benchmarked and its
   rejected designs documented. This matters for credibility with the DPReview
   audience that distrusts trials ("known bugs I won't discover in 30 days").

### Native vs. Tauri: the evidence does not currently favour native

A reasonable expectation is that Lap's Tauri/web-component UI cannot match a
native one for smoothness, and that this is a durable phig advantage. **On the
evidence gathered so far, that is not established — and the one direct comparison
available runs the other way.**

| | Lap (Tauri + Vue) | phig GTK M0 (native C++) |
|---|---|---|
| Reported scroll | M4 user: 1500 images "smooth as silk" at moderate speed; minor redraw artifacts only when scrolling fast | **~95% of frames over 16 ms** warm cache; worst frame 66.7–83.3 ms |
| Worst case | not reported | **~1.3 s** miss-heavy scroll |
| Thumbnailing | ~700 MB in ~15 s (M4) | **~13 files/sec**, HDD-seek-bound |
| Startup | not reported | 73.8 ms median (passes its 500 ms budget) |

Caveats that genuinely matter: different hardware (Apple M4 vs. a 12-core box with
HDD-backed photos), different file sizes, and Lap's numbers are user reports rather
than instrumented measurement. **This is not a like-for-like benchmark and should
not be treated as one.** phig's numbers are its own honest instrumentation;
`PHIG_MEASURE=1` exists precisely because this project measures.

But two things follow regardless:

1. **phig's GTK client currently does not hold 60 fps**, by its own README's
   admission — "the grid does not hold 60 fps" — and four tuned constants (page
   radius, loader threads, cache size, placeholder count) were each measured to
   make no difference to warm-cache frame time. The diagnosis was main-thread
   texture upload and widget work. That is a real, unsolved GTK problem, not a
   toolkit-choice advantage.
2. **Users do not perceive toolkit; they perceive frame drops and latency.** Two
   commenters did object to web UIs on principle ("I really hope this is not an
   electron app," a request for WinUI), so a native-ness constituency exists — but
   it did not stop 685 upvotes, and nobody in six threads complained that Lap felt
   slow.

**The honest position:** native *can* be faster and gives better memory behaviour
(phig's 366–670 MB peak RSS is itself high), but "native therefore smoother" is a
hypothesis phig has not yet demonstrated and Lap has provisionally falsified on
the axis users notice. Until the GTK client clears its own 60 fps bar, treating
toolkit choice as a differentiator is reasoning ahead of measurement — the exact
failure mode the project's own conventions warn about ("performance questions have
only yielded to measurement").

**Not resolvable yet (decided 2026-08-05).** An earlier version of this section
recommended benchmarking phig's GTK client against Lap on the same corpus. That
was premature and is withdrawn: M0 is a read-only thumbnail wall with no search,
selection, detail view, or tagging UI, so a comparison against a shipped
application is uninterpretable. A loss would not distinguish toolkit cost from
missing work from the HDD-backed corpus; a win would not survive adding the
missing features.

**What to do instead:** treat phig's own instrumented numbers as the bar to clear
— `PHIG_MEASURE=1`, ~95% of frames over 16 ms warm, worst frame ~1.3 s. Fix the
diagnosed cause (main-thread texture upload and widget work) and re-measure
against M0's own baseline. Competitive comparison becomes meaningful only once the
client does enough to be compared.

**In the meantime, do not claim native smoothness as a differentiator** — in
positioning, plans, or design rationale. It is currently an untested hypothesis
that phig's own measurements contradict.

### The GPL point, which cuts both ways

Lap is **GPL-3.0-or-later**. phig has no license file yet — worth deciding
deliberately.

GPL means Lap cannot be embedded in proprietary software, and its code cannot be
lifted into a differently-licensed project. If phig ever wants commercial
licensing, an OEM story, or permissive embedding of `libphig`, that is a real
axis of difference — a C ABI under a permissive license is something Lap
structurally cannot offer.

### "Hard to compete with free/open source" — the honest read

Correct, on the axis Lap competes on. It is free, GPL, cross-platform, has a
modern UI, an active author shipping monthly, and 685 upvotes of goodwill. **phig
will not win a feature race with it, and should not try.** Lap has already closed
most of the gaps this document identified as phig advantages, in about a year.

What is left is narrow but real:

- **phig is infrastructure; Lap is an application.** The CLI + C ABI is a
  different product category. Nobody scripts Lap, embeds Lap, or builds a web
  client on Lap. That is phig's actual moat, and it is the thing the six threads
  never asked for — which is the tension worth resolving deliberately rather than
  by drift.
- **Provable completeness and measured performance** on dedup, where Lap is
  approximate.
- **Recoverability across sessions** — but only once `phig log`/`phig undo` ship.

The uncomfortable conclusion: if the goal is to win the general-user desktop photo
manager market, Lap is a year ahead with a better client and the same pitch. If
the goal is the engine that other things are built on — CLI users, a web client,
an Elixir consumer, automated pipelines — Lap is not a competitor at all, and none
of the six threads speak to that market.

## Is "infrastructure" a viable market? (investigated 2026-08-05)

Prompted by the conclusion above that phig-as-engine competes with nothing in the
six threads. **Answer: scripting demand is real and large; the specific niche
phig would occupy is small and mostly already served.** Details below, because the
distinction matters.

### Scripted photo workflows are genuinely widespread

Not a hypothetical. Measured GitHub stars, 2026-08-05:

| Project | Stars | What it is |
|---|---|---|
| immich-app/immich | 109,780 | the GUI incumbent, for scale reference |
| rclone/rclone | 58,962 | scriptable sync; used in photo backup cron jobs |
| qarmin/czkawka | 32,498 | dedup, CLI + GUI |
| libvips/libvips | 11,550 | embeddable image processing library (LGPL) |
| simulot/immich-go | 6,655 | third-party CLI for Immich; Takeout/iCloud import |
| exiftool/exiftool | 4,927 | the metadata tool (GitHub mirror only — real usage is far higher, it ships in every distro) |

ExifTool in particular is the load-bearing evidence: a deep tutorial ecosystem
exists for exactly the jobs phig does. `exiftool '-Directory<CreateDate' -d
"/photos/sorted/%Y/%Y-%m" -r /photos/unsorted/` is phig's `mv --format` in one
line, and people write blog posts, gists, and bash wrappers around it. Cron-driven
photo pipelines are a normal thing self-hosters build.

**So the audience exists.** It simply never appeared in the six threads because
those threads asked "what app should I use," not "what do you automate with."

### But the niche is crowded from both sides

- **ExifTool owns metadata + rename + folder-filing.** Two decades old, every
  format, in every package manager, with a huge recipe corpus. phig's date cascade
  and format strings are *better designed* for the specific job, but "better than
  ExifTool at filing photos by date" is a hard sell against something already
  installed and known.
- **Czkawka owns standalone dedup** at 32.5k stars, CLI included.
- **Immich owns the API-ecosystem position.** This is the finding that most
  directly undercuts the plan. Immich ships an OpenAPI-generated REST API, a
  dedicated `api.immich.app` docs site, and an explicit invitation for third
  parties to extend it — with a "Community Projects" directory. immich-go at 6.6k
  stars is proof the ecosystem is real. **If a developer wants a photo backend to
  build on, Immich already is one**, with mindshare phig cannot match.
- **Commercial OEM licensing exists and is occupied.** ACDSee sells Technology
  Licensing / SDK / OEM for DAM with AI keywords and face recognition; IMG.LY
  sells an embeddable creative SDK to DAM vendors. So the business model is proven
  — but it is enterprise B2B sales, not a developer-adoption play, and it is a
  completely different company to run.

### What is genuinely unoccupied

A narrow slice, stated precisely: **an embeddable, permissively-licensed,
in-process photo-management engine with a stable C ABI.**

- Immich's API requires running a server, a Postgres, and HTTP round-trips. Not
  embeddable in a desktop app.
- ExifTool is a Perl process you shell out to per file. No library, no index, no
  dedup, no faces.
- Czkawka is dedup only.
- Lap has no library target and is GPL.
- libvips is the closest structural analogue — an embeddable image library that
  many products build on — but it is *processing*, not *management*: no catalog,
  no dedup index, no face store.

phig's `core/` + `libphig` is that missing piece, and it is a real gap. **The
question is whether anyone is looking for it.** No evidence found either way; the
searches surfaced no one asking for an embeddable photo-management library. That is
absence of evidence from a handful of queries, not proof of no demand — but it is
the second thesis in this document to reach that state.

### The sustainability problem

libvips is a "volunteer project" that needed OpenCollective funding for one year
so its maintainer could do GUI work. ExifTool and LibRaw are similarly
single-maintainer with no visible funding model. **The infrastructure-library
position has a known outcome: essential, widely used, and financially thin.**
That is fine as a goal, and it is worth naming rather than discovering.

### Assessment

Infrastructure is viable as a **differentiator** and weak as a **market**:

- As a moat against Lap: real and structural. Lap cannot be scripted or embedded,
  and GPL forecloses proprietary embedding.
- As a primary market: unevidenced demand, three incumbents holding adjacent
  ground, and the closest analogues are unfunded volunteer projects.

The practical reading is that these are not exclusive. The CLI and `libphig`
already exist and are near-free to keep; they cost nothing to maintain as a
differentiator while the GUI is what actually competes. **Do not restructure the
project around infrastructure as the primary market on this evidence. Do keep the
CLI and ABI, and say so in the positioning.**

### 5. Dedicated duplicate finders — a category phig partly competes with

Also unmentioned by the buying guides. People solve "I have duplicate photos"
with a single-purpose tool, not a photo manager.

- **Czkawka** — free, open source, **written in Rust**, cross-platform. Widely
  recommended; reputed to beat Python-based tools on hundreds of thousands of
  files. Finds duplicates, similar images, empty folders, broken files.
- **dupeGuru** — free, open source, fuzzy matching, long-standing.
- **AllDup** — free, Windows-only, detailed filtering, dated UI.

**Why this matters to phig:** duplicate detection is one of phig's strongest
engineered assets (160k in 1.7s), but it is not by itself a product — free
tools already own the standalone use case, and Czkawka in particular is fast and
credible. Dedup is a *feature* that strengthens a manager; it is not a wedge.

Sources: [Czkawka](https://github.com/qarmin/czkawka),
[dupeGuru alternatives](https://alternativeto.net/software/dupeguru),
[comparison](https://www.saashub.com/compare-dupeguru-vs-czkawka).

## Recurring complaints in the field

Compiled from the "reasons to avoid" / cons columns across all three articles.
Four repeat, and phig's position on each differs sharply.

| Complaint | Who it hits | phig |
|---|---|---|
| **Forced catalog import** — Lightroom Classic's top con; ACDSee's whole "best import-free" positioning; Excire "can take a while to import all your content" | Lightroom Classic, Zoner, Excire | Scans in place. mtime-skip on rescan. The DB is a queryable index, not a container. |
| **Subscription** | Adobe, Capture One, Zoner, Mylio ($99–119/yr) | None. |
| **Dated / overwhelming / cluttered UI** — digiKam's exact con; XnView "utilitarian"; ACDSee "cluttered" | the free and cheap tiers | **Worst area.** `gtk/` M0 is a read-only thumbnail wall that fails its own 60fps bar (~95% of frames over 16ms warm). |
| **"Requires technical knowledge"** — the *only* con Aiarty names for both Immich and PhotoPrism | Immich, PhotoPrism | No Docker, no server, no reverse proxy. But it needs a terminal. |

The pattern: the self-hosted tier's single named weakness is setup difficulty;
the paid desktop tier's is import friction plus rent. **Nobody in this field
ships a local-first organizer that installs like a normal application.**

## The P2P sync opportunity

The strongest differentiator identified so far, and it is unbuilt.

**Mylio** is the only product across all three articles doing device-mesh sync
with no mandatory cloud, and Aiarty rates it well for exactly that. Its
weaknesses are structural, not cosmetic:

- The Vault machine must be awake and running the app or nothing syncs. Desktop
  asleep → phone does not update.
- "Vaults," "Optimized Images," and "Sync Policies" reportedly take an afternoon
  to configure correctly.
- Users report paying and still failing to get devices to sync.
- [$99.99–119/year](https://sourceforge.net/software/product/Mylio/).

**Immich** is the other half. Its mobile apps work, and remote access
[does not strictly require a VPN](https://docs.immich.app/guides/remote-access/) —
reverse proxy with TLS is supported. But every path assumes an always-on server
the user administers and exposes. That is hub-and-spoke wearing self-hosted
clothing.

**The unoccupied position:** phone-to-laptop photo sync with no server, no
account, no cloud, and no always-on hub. Two devices on the same Wi-Fi sync
directly. Not a thing Mylio does badly — a topology neither Mylio nor Immich
has.

### Demand evidence: still none (2026-08-05)

Across three primary sources — a DAM thread, a leaving-Google-Photos thread, and
a general "how do you organize" thread — **not one participant asked for
serverless P2P sync.** The r/AskPhotography poster explicitly deprioritized
cross-device viewing. Cloud demand that does appear is for *backup* (S3 Glacier,
Backblaze, offsite NAS), which is a different job.

That is absence of evidence from a small sample, not proof of no demand. But the
thesis was reasoned from a supply gap, and three chances to corroborate it from
the demand side produced nothing. Treat it as unvalidated until a source
supports it.

### Sampling-bias correction to the finding above (2026-08-05)

**"No evidenced demand" overstates what the evidence supports, and the sampling is
biased against finding this particular demand.**

Every source in this document is a forum or subreddit where people who already own
and discuss photo-management software talk to each other — r/unRAID, r/photography,
DPReview, r/macapps. **Non-technical people who cannot get photos off their phone
do not post in those places. They ask someone they trust.** Searching those forums
for this demand is looking in the one place it is structurally unlikely to appear.

Countervailing firsthand evidence from phig's author: people who are less
technically inclined ask *constantly* how to get their photos from phone to laptop.
That is direct observation of the target audience, from a channel no forum search
reaches, and it outranks the absence found above.

**The corrected claim:** no demand for serverless P2P sync appeared *among people
who already run photo management software*. That is much narrower than "no demand,"
and it says nothing about the larger, quieter audience who never gets as far as
choosing a tool.

**What remains true and unchanged by this correction:**

- The transfer job is solved several times over, cheaply — PhotoSync (under $5,
  all four platforms, local Wi-Fi, no cloud, no server), AirDrop inside Apple,
  and Google Photos/iCloud by being preinstalled. The person asking is not blocked
  by absent software.
- So the barrier is **discovery and trust, not capability** — and discovery is
  brutally hard to win as an unknown entrant, because "which app should I use" is
  answered by whoever the asker already trusts. That currently is phig's author,
  which does not scale.
- The technical obstacles are real and explain why the frictionless version does
  not exist: iOS background-execution limits mean a sleeping phone cannot reliably
  sync; app-store presence requires developer enrollment, review, and two mobile
  codebases from zero; local network discovery is fragile across router configs.
  Mylio has a funded team and still draws complaints that devices will not sync.
- The genuinely unoccupied position is still the *combination* — transfer plus a
  real organizer, no cloud, no server, no subscription. Unoccupied does not imply
  profitable when the transfer half is a commodity and the audience pays for
  nothing today.

**The cheap decisive test, and it needs no code.** phig's author has direct access
to the exact audience these forums cannot reach. Over the next several times
someone asks, establish three things:

1. What have they already tried, and why did it fail them?
2. Would they pay anything at all, and roughly how much?
3. **Is the pain the transfer itself, or the mess afterward?**

Question 3 decides the product. If the answer is consistently "the mess
afterward" — thousands of unsorted photos, duplicates, can't find anything — that
is phig's engine, and transfer is merely the on-ramp. If it is "I just want the
files on my laptop," PhotoSync already wins and there is no business here.

Ten conversations, no build. Until they happen, this thesis is **unvalidated in
both directions** — not refuted, as the section above implied.

### Correction to the above (2026-08-05, same day)

**The transfer half of that position is already occupied.** Overstated on first
pass; the qualification matters.

- **PhotoSync** ([photosync-app.com](https://www.photosync-app.com/features.html))
  does direct phone→computer transfer over local Wi-Fi with no cloud and no
  server — "your photos & videos never leave your local Wi-Fi network." Native
  iOS, Android, Windows, Mac; Linux via SFTP/FTP/SMB. Free companion desktop
  app, mobile free with IAP, reportedly under $5. Auto-transfer triggered by
  Wi-Fi, location, schedule, or charging. Preserves EXIF/GPS/IPTC.
- **Syncthing + Immich** is a widely-documented pairing: Syncthing drops a plain
  copy of the camera roll onto a machine, Immich indexes that same folder.
  People are already assembling the serverless-ingest experience from parts.

**What PhotoSync does *not* do:** no deduplication, no face recognition, no
organizing, no search. It is transfer and backup only.

**So the revised claim** — narrower and still defensible: the gap is not
serverless *transfer*, it is serverless transfer **integrated with a real
organizer**. Today you either get transfer without management (PhotoSync), or
management that assumes an always-on server (Immich, PhotoPrism), or a mesh with
a mandatory awake hub and a subscription (Mylio). Nobody ships ingest +
dedup + faces + search with no server and no account.

That is a genuinely smaller claim than "nobody does serverless P2P sync," and it
should be stated the narrow way. It also means the sync work is less
differentiating on its own than first assessed — PhotoSync proves the transport
is solved and cheap. The value is in what happens to the photos after they land.

**Groundwork phig already has:** SHA256 on every file, content-addressed
thumbnails, an append-only operation log with per-entry restore snapshots, and
forward-only schema versioning. The activity log is the skeleton of a sync
protocol; what is missing is conflict resolution and a wire format, not a new
data model.

**Honest cost:** two mobile clients from zero. P2P sync on mobile fights
aggressive background-execution limits on both iOS and Android. The current GUI
is one incomplete milestone on one platform; five first-class clients is a
multi-year commitment.

## Differentiators, ranked by defensibility

### Strong — rare in the field and already built

- **RAW + JPEG as one photo.** No product in these three articles mentions it.
  Real prosumer pain, and phig's three-tier pixel source (sidecar JPEG →
  embedded preview → LibRaw) is validated on all 177 CR2 files in the reference
  collection — 62 of which OpenCV alone could not decode. **Strengthened
  2026-08-05:** in r/macapps, Lap's lack of RAW support drew "no RAW support kinda
  ruins the whole idea" as the top comment, from a general audience, forcing a fix
  within a month. RAW is a gating requirement, not a prosumer extra — and RAW+JPEG
  pairing is also implemented in Lap (`group_raw_jpeg_pairs`) — see the source audit.
- **Duplicate detection that is actually engineered.** 160k images in 1.7s
  versus 150s brute force, with a rejected BK-tree and two measured fallback
  thresholds documented. Everyone else lists "duplicate detection" as a bullet.
  These numbers are publishable.
- **Scan-in-place, no catalog.** ACDSee built a category label on this and DCW
  gave it the "best import-free" slot. **Downgraded 2026-08-05:** advantage
  against Lightroom, Excire, and Zoner; **parity against Immich**, whose external
  -library mode is the most-endorsed configuration in the r/unRAID thread and
  explicitly "you don't have to touch, copy, move do ANYTHING." Still a
  differentiator, but not against the strongest competitor.
- **Correct dates.** The EXIF → sidecar → mtime cascade fixes Google Takeout
  exports (the general-user migration case) and EXIF-stripped prosumer files.
  Underrated: it is the difference between a migration that works and one that
  files ten years of photos under today.

### Strong but not yet deliverable

- **Data safety as a stated contract.** Trash-by-default, `RENAME_NOREPLACE`
  no-clobber, fsync'd cross-device copies, restore snapshots, advisory writer
  lock. No competitor markets this, and "will not lose or clobber your photos"
  is exactly the promise a local-first tool needs to displace a cloud people
  distrust but which has never eaten their library. **Caveat:** the activity log
  has no viewer and no `undo` (deferred in plan 020), so the data is recorded
  but unreachable. The promise cannot currently be delivered to a user.

### Reassessed 2026-08-05: local AI provenance is a differentiator

Originally filed under "weak — do not compete here," on the reasoning that every
product claims face recognition and Google Photos does natural-language search,
so AI capability is table stakes. **That reasoning holds for capability and was
wrong about positioning.**

The r/AskPhotography thread shows the axis is not how good the AI is but
**where it runs and what it sends**. The user's own framing: AI is acceptable if
it is "just a fancy programme that doesn't communicate with anything and doesn't
collect any data." A commenter recommended digiKam on precisely those grounds —
optional, off by default, locally trained, predating modern AI models. And the
whole premise of the question was distrust of "AI powered workflows."

phig's face recognition is local ONNX models (YuNet + SFace), downloaded once,
no telemetry, no account, no network at inference. **That is the exact property
this audience is asking for and cannot easily verify in a cloud product.**

Still true, and unchanged: do not compete on *model quality* or semantic search
breadth against Google's budget. phig has no clustering, so it cannot offer
"here are 40 unnamed people to name" — and both threads asked for something like
that (the general user described their dad's software auto-detecting and grouping
new faces for review). **Face clustering plus a review UI is the gap**, not a
better embedding.

## Single product for both audiences?

Question raised 2026-08-05: can one product serve prosumers and general users?

**Answer: yes, and this is now evidenced rather than reasoned.** Originally
argued from analogy (ACDSee and ON1 do it from one binary). The two primary
sources settle it better than the analogy did.

**The two audiences independently asked for the same five things:**

| Requirement | Prosumer (DPReview, leaving Lightroom) | General user (r/AskPhotography, leaving Google Photos) |
|---|---|---|
| Runs locally | implied throughout | "runs **entirely locally**" |
| One-time payment | "not too expensive" | "happy to pay for it, preferably a **one-off cost**" |
| No editor needed | wanted a DAM, already owns DXO + Affinity | "don't need... complicated editing tools" |
| Easy to learn | "**easy to learn**, stable" | "an **intuitive UI**" |
| Organizing that beats folders | metadata/keywording depth | tag one photo as dog *and* hike, find it by either |
| Culling / comparison | compare similar shots, cull on import | (recommenders surfaced Photo Mechanic for bulk culling) |

Different vocabulary, same product. Neither mentioned cloud, sync, sharing, or
mobile. That is a stronger basis for one product than the ACDSee analogy, because
it comes from buyers rather than from vendors who happen to ship one binary.

The earlier analysis of what splits still stands: prosumer-only surface (RAW+JPEG
pairing, `--camera`, sharpness filters, the CLI) costs the general user nothing
because it is invisible unless sought.

- **Shared, no divergence:** scan-in-place, dedup, date cascade, faces, tags,
  galleries, trash-by-default, P2P sync. Most of the product.
- **Prosumer-only:** RAW+JPEG pairing, `--camera`/`--make`, sharpness filters,
  the CLI. These cost the general user nothing — invisible unless sought. RAW
  +JPEG pairing helps a user with no RAW files by never triggering.
- **The one real conflict: the entry point.** Prosumers want a terminal and
  scriptability; general users need an installer and to never see a flag. That
  is a packaging split, not a feature split, and phig is already layered for it
  (`core/` → `libphig` → clients).

**Two caveats.**

1. **Interface discipline has to hold under pressure.** The root `CLAUDE.md`
   rule — non-GUI logic goes in `core/` behind a libphig entry point, never
   duplicated in clients — stops being hygiene and becomes the thing holding the
   product together. Two audiences is exactly the scenario that tempts a filter
   that is "just for the GUI."
2. **libphig lags `core/`.** The long-running operations (scan, organize,
   remove, faces) exist in `core/` and are proven by the CLI, but have no ABI
   surface, so a GUI cannot run them without violating the interface rule. Plan
   031 covers this. Smaller and lower-risk than a design change — but it gates
   any general-user GUI.

**Real risk: the pitch, not the product.** "Local-first photo manager for RAW
shooters" and "get off Google Photos without the cloud" are different landing
pages, search terms, and first-run experiences. ACDSee gets called both
"cluttered" and "not a professional solution" in the same review — the tax on
being both in one UI. One product with two front doors is feasible. One pitch
probably is not.

**Working recommendation:** one engine, one GUI, segmentation by defaults. GUI
opens to a timeline with faces and dedup surfaced; RAW/sharpness/camera filters
present but off the primary path; CLI unchanged. Prosumer features as depth, not
as a mode.

## The gap that decides this

phig's engine is mature and unusually well-measured. Its interface is a
terminal. Every product in these three articles is a GUI, and the closest free
competitors are criticized specifically for GUI quality — the one area phig is
currently weakest rather than strongest.

**Revised after six primary sources (2026-08-05).** The original version of this
section said P2P sync was the right wedge and the GUI merely gated it. Both halves
need correcting:

1. **The P2P wedge is unvalidated — in both directions.** Six sources produced
   zero requests, but all six sample people who already own photo software, so the
   absence is weak evidence (see the sampling-bias correction). What is solid:
   PhotoSync already does the transport for under $5 on all four platforms, and
   r/unRAID shows it in production paired with Immich. The unoccupied slice is
   transfer *integrated with* an organizer.
2. **The GUI is not a gate in front of a differentiator — it is the
   differentiator, and it is contested.** Lap shipped a modern local-first
   folder-first manager with local AI, took 685 upvotes and comments like "exactly
   what I was looking for past 10 years," and is iterating monthly. Its engine is
   thinner than phig's on every measured axis (dedup, RAW, robustness, video), and
   that has not mattered to its reception. phig's GTK M0 does not clear the bar Lap
   already cleared.

**The honest summary:** phig has the best engine and the worst client among its
real competitors. Every advantage catalogued in this document — the banded phash
index, the three-tier RAW pixel source, the restore-snapshot activity log,
scan-in-place reconciliation — is invisible to a user who cannot see their photos
without a terminal. The most-cited buying criterion across all six sources is
"easy to learn" / "intuitive UI."

**The one asset that is both differentiating and defensible against Lap
specifically** is robustness under real-world mess: Lap hangs permanently when
files are deleted outside it (author's remedy: delete the database), while phig
reconciles stale rows, re-links by sha256, and keeps restore snapshots. That is
the data-safety pitch — still undeliverable without `phig log`/`phig undo`, which
makes shipping those a market requirement rather than a nice-to-have.

## Capability gaps with repeated demand across sources

Ranked by how many independent primary sources asked for it. These are user-voiced
wants phig does not currently serve, not speculation.

| Gap | Sources | Notes |
|---|---|---|
| **Face clustering + merge/split + review UI** | 4 — r/AskPhotography (dad's software auto-grouped new faces for review), r/Windows11 (threshold slider, "manual corrections are inevitable"), r/macapps ("automatic face recognition and grouping"), DPReview (People tab) | The most-requested thing phig lacks. It has embeddings, KNN, and propagation but only one-photo-at-a-time naming. No clustering means no "here are 40 unnamed people." |
| **Metadata that outlives the tool** (write tags/ratings to XMP/IPTC) | 3 — r/Windows11, r/macapps (×2, incl. "travel with the file if I send it to a contractor"), DPReview (XMP nuance) | phig's tags live only in `phig.db`. **Inconsistent with phig's own stated principle** that files are durable and the DB is a rebuildable index. Deserves a plan. |
| **Star ratings (1–5) + filter/threshold** | 2 explicit, 3 implied — r/macapps (Cmd+1–5 then cull 1–3), r/AskPhotography (star, delete 1s and 2s), DPReview (rate/cull in Viewer) | phig has **no rating scale at all** — tags, galleries, and a binary favorite. Ratings are the specific mechanism users name for culling. Small, concrete. |
| **Culling / side-by-side compare** | 3 — DPReview (light table, 8-up, histogram), r/AskPhotography (Photo Mechanic for bulk culling), r/macapps (split-view compare) | phig's phash grouping + `blur_score` cover the *selection* half; there is no compare UI. Several call a RAW histogram essential. |
| **Geotag editing / location search** | 2 — r/Windows11, r/macapps (prefer existing metadata over recomputed) | phig parses GPS incidentally into the JSON blob but never indexes or searches it. Plan 013 is planned-only. |
| **Portable / configurable DB location** | 1 strong (multiple askers in r/Windows11) | Wanted for external drives *and* for syncing the DB across machines via Syncthing. phig's `--catalog` flag already allows this; XDG default is fine. Cheap win. |
| **Background/incremental indexing without blocking the UI** | 2 — r/Windows11 (subfolders inaccessible until scan completes), r/macapps | Directly relevant to plan 031: a GUI running scan needs progress and cancellation, and must stay usable. |

## digiKam's large-library performance — unresolved, possibly an opening

The closest free competitor, and the reports conflict (all secondhand via search
summaries of DPReview threads; none read directly):

- A 2026 review claims digiKam handles 100,000+ images with "zero search latency."
- One user called it "the slowest image management software" they had tried —
  ~5 hours to load about half of 27,000 images into its database.
- Another, just under 100,000 photos, understood that to be "close to the limit
  that digiKam's SQLite can handle well."
- Consensus regardless: powerful, free, strong community, worth trying.

Both extremes cannot be true. The distinction that likely explains it is
**initial ingest time versus steady-state query time** — phig has the same shape
of cost (~13 thumbnails/sec, HDD-seek-bound) and would look equally slow on first
scan while being fast afterward.

**If** digiKam genuinely degrades near 100k on SQLite, that is a concrete
opening: phig's 30k reference collection counts in 1.3 ms and deep-pages in
2.6 ms on the same SQLite, and the duplicate index handles 160k hashes in 1.7 s.
**But this is unverified**, it is the kind of claim this project requires
measurement for, and the honest comparison would need digiKam run against the
same corpus. Do not put it in any pitch until measured.

## Open questions

- Target platform scope: is the honest near-term goal Linux/macOS/Windows
  desktop plus one mobile sync client, rather than five first-class clients?
  Note the DAM thread cites Win+Mac as the coverage that matters and never
  mentions Linux.
- **Is there evidence of demand for serverless P2P photo sync?** Still open, and
  **not** resolved negative — an earlier version of this line said it was, which
  overstated the evidence. Six threads produced zero requests, but all six sample
  people who already own photo software; the audience that cannot move photos off a
  phone does not post there. Firsthand author observation says they ask constantly.
  Resolve it with ten conversations, not a build — see the sampling-bias
  correction, especially question 3 (transfer, or the mess afterward?).
- **Is culling/similar-shot review the better wedge than sync?** Voiced in three
  threads. phig's phash grouping plus `blur_score` cover the selection half.
  Open: does it need a compare UI with a RAW histogram, and **does it need star
  ratings** (the mechanism users name) rather than the binary favorite phig has?
- **The decisive question is now GUI, not features.** Lap cleared the bar with a
  thinner engine and a good UI. Does phig's GTK client get to that standard, and
  what is the cost? Everything else in this document is downstream of that answer.
- **Should tags be written to XMP/IPTC** so they survive the tool? Asked in three
  threads, and phig's DB-only tags contradict its own "files are durable, the DB
  is a rebuildable index" principle.
- Does the data-safety pitch require `phig log` and `phig undo` shipping first?
  (Leaning yes — an unverifiable safety claim is worse than none.)
- Pricing: Excire's $189 one-time is the closest comparable. Open-source with no
  price is a different strategy and forecloses the Excire comparison.
- What does "installs like a normal application" mean per platform, and what is
  the packaging cost?

## To investigate

- ~~Benchmark phig's GTK client against Lap~~ — **withdrawn 2026-08-05.** M0 is
  not mature enough for the comparison to mean anything; see the native-vs-Tauri
  section. Revisit once the client has search, selection, and a detail view. Until
  then the bar is M0's own instrumented baseline, not a competitor's.
- ~~Lap in depth~~ — source audit done, see above. Still worth reading its GitHub
  **issues** (not just code) as a public roadmap and a list of what its users hit.
- **Whether anyone actually wants an embeddable photo-management library.** The
  infrastructure thesis rests on an unoccupied niche with no evidenced demand.
  Where to look: forums for people building photo features into other apps,
  Immich's Community Projects directory (what did people build, and what did they
  wish the backend gave them?).
- **Whether other Picasa-replacement projects are further along.** Lap is one of
  several; Phototheca, Rollfilm, ImageFan Reloaded, FlowVision, Eagle, and
  FastStone all surfaced. Nobody has surveyed this cohort.
- **Measure digiKam** against the same corpus phig uses, rather than trusting
  conflicting forum reports. See the section above.
- **Culling tools as a category** — FastRawViewer, Photo Mechanic, Fast Picture
  Viewer (reportedly dead; developer under a non-compete). The DAM thread suggests
  people happily pay for a *separate* culling tool, which is a category phig's
  engine could enter. Pricing and reception unresearched.
- ~~Immich's trajectory~~ — done, see section 3. Recheck periodically; it moves
  fast.
- Whether any product does serverless P2P photo sync outside this field
  (Syncthing, Resilio). PhotoSync already found — see the P2P correction.
- **Photo Supreme and iMatch in depth** — feature lists, what their users ask for
  and do not get. The photools.com and idimager forums are the place, and both are
  likely fetchable (not Cloudflare-hard).
- Excire Foto's reception among actual users, not reviewers. Closest analogue to
  a phig product at $189.
- **Media servers as an architectural analogue** — Jellyfin, Plex, Emby. They
  solved the exact split plan 052 is building: a server that owns the storage,
  native clients per platform, and a web view over the same library. Their users
  have years of public complaints about it. What did they get right, what do
  users hate, and how do they handle the problems 052 has to solve anyway —
  transcoding policy, authenticating media requests a browser makes without
  headers, and clients that go offline. Cheapest available source of hard-won
  design constraints, and none of it has been read.
- **Re-survey the field with a server in scope.** The existing survey assumed a
  desktop product. For the service-plus-clients shape the comparison set is
  PhotoPrism, Immich, Piwigo, Nextcloud Memories and LibrePhotos, and only
  Immich has been read. See the 2026-08-08 section.

## Decision: not pursued commercially (2026-08-05)

**phig is not being pursued as a business.** Recorded here with the reasoning so
that a future reader — including its author — can check whether the conditions
that produced this conclusion still hold.

The goal being tested was income, not ownership. Against that goal, all three
plausible paths fail on specifics found in this document:

1. **Paid product.** Every tool the market actually recommends is free — digiKam,
   Czkawka, XnView, FastStone, Immich, PhotoPrism, Lap. The one surviving paid
   tier is DAM-only at $129–189 perpetual (Photo Supreme, iMatch, Excire), whose
   buyers switch roughly once a decade (one user on Photo Supreme "since apx
   2007"). Hard to win, then durable — the worst shape for a new entrant needing
   early revenue. The segment that would pay means competing with Adobe's
   distribution and Excire's two decades of AI work; the segment that shares
   phig's values expects free and open source.
2. **Open source plus sponsorship.** libvips is the honest comparison: 11,550
   stars, genuinely essential, and its maintainer needed OpenCollective funding
   for *one year* of work. ExifTool and LibRaw are single-maintainer with no
   visible funding. That is the good outcome of this path.
3. **Infrastructure / embeddable engine.** No evidenced demand after targeted
   searching, and Immich already holds the API-ecosystem position (OpenAPI spec,
   `api.immich.app`, a community-projects directory, immich-go at 6.6k stars).

Timing compounds it. Lap shipped a year earlier — free, GPL, cross-platform,
actively maintained, with a working client — and AI compresses the build time for
the next entrant. phig would enter late, against free, with its weakest surface
(the client) being the first thing users judge.

**On craft as a differentiator.** The counter-argument was that 24 years of
experience yields polish others cannot match. That capability is real; its
commercial decisiveness is not. Lap is the test case: approximate HNSW similarity
with unmeasured thresholds, a hang-on-external-delete bug whose remedy is deleting
the database, no completeness guarantee — and 685 upvotes with "exactly what I was
looking for past 10 years." phig's dedup is provably complete with published
numbers; nobody across six primary sources asked for that. Polish is a tiebreaker
between products a user already wants, not a reason to pay when a free option
exists.

**What the work is still worth.** The engine is strong and the discipline behind
it is scarce — a BK-tree rejected after measurement, a strided sample that caught
a 200× misread, the busy-timeout ordering bug, `RENAME_NOREPLACE`, trash by
default. That is judgment about failure modes under real data, which is more
valuable as AI generates more plausible-but-wrong code, not less. It is also
invisible in a screenshot and unmentioned in every thread collected here. Its
return is better realized as demonstrated capability than as licence sales.

**Conditions that would change this conclusion:**

- A paying audience appears for something phig uniquely does (provably complete
  dedup, RAW+JPEG pairing, cross-session recoverability) — evidenced by demand,
  not inferred from a supply gap. The embeddable-engine thesis reached "no
  evidenced demand" and should not be revived without it.
- **The P2P/mobile-transfer thesis is the one live lead**, and it is untested rather
  than refuted. Every source here samples people who already own photo software, so
  it cannot see the audience that gets stuck moving photos off a phone — an audience
  the author reports asking constantly. Ten conversations with real askers would
  settle it, and the deciding question is whether their pain is the transfer or the
  mess afterward. If it is the mess, that is phig's engine and this decision should
  be reopened. See the sampling-bias correction.
- Lap stalls and no comparable free replacement emerges.
- The DAM incumbents (iMatch Windows-only, Photo Supreme) lose their audience
  without a free successor taking it.

**Method note for next time:** six forum threads and one source audit answered
this in an afternoon — after the engine was built. The sequence, not the code, was
the expensive part.

## Service + native clients + web view over a local database (2026-08-08)

**This is a position, not a finding. No new sources were read.** It is recorded
so a later reader can see what was believed on this date and check whether it
survived contact with evidence.

phig is building a server — plan 052, `phigd` over the collection, with native
clients per platform (047) and a web view. Section 3's recommendation 1 says not
to target the self-hosted NAS segment as a primary market, because Immich owns it
and phig loses on every axis that audience named. **That recommendation stands
as positioning and is not why 052 is being built**; its drivers are compute
offload and remote access for one user. But the author's position is that there
is a good bet in this shape and it should be tested rather than assumed dead.

**What might be differentiated.** Not "self-hosted photo server" — that is
Immich's, decisively. The shape is a service, native clients and a web view
*around a local database that is an index rather than a container*. Immich's
model is a server that owns the library; phig's is files that stay where they
are with names unchanged, a rebuildable index, and an append-only activity log
with restore snapshots. Recommendation 2 already identified transparency and
recoverability as the one genuine opening, and noted it was unverifiable without
`phig log`/`phig undo` shipped and a DB-rebuild story. 052 gives that opening a
delivery vehicle it did not previously have — but does not supply the missing
proof, which is still those two features.

**Second possible distinction: nobody in the field has all three.** Immich has
web and mobile with an incomplete desktop client. Lap is desktop-only with no
server. digiKam is desktop-only. Photo Supreme and iMatch are desktop products
whose remote story is a separate paid server (IMatch Anywhere). The
multi-user/remote finding above already recorded that this is asked about and
barely served.

**Unresearched, named here so it is not later mistaken for evidence:** whether
anyone wants native *and* web over one collection, whether index-not-container is
legible to a buyer before they have lost data, and whether a single-user
remote-access product has any audience beyond its author.

This does not reverse the 2026-08-05 decision not to pursue phig commercially.
It is a second live lead alongside the P2P/mobile-transfer thesis — untested
rather than refuted — and it should be researched before it is either acted on
or dropped.
