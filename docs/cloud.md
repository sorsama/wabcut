# Cloud projects, review links and stock media (0.7)

Three things for work that leaves your own computer, none of which needs a Wabcut account or a Wabcut server (there is none):

* **Cloud projects on your own storage**: keep a project in a folder your sync service mirrors, on a WebDAV server such as Nextcloud, or in an
  S3-compatible bucket, and edit it from more than one computer without anyone's work being lost.
* **Review links**: send a draft to a client as a web page they open in a browser, where they comment on exact frames; their comments come back
  as review notes at the right frames.
* **Stock media**: search free videos and photos on Pexels and Pixabay with your own key, add one to the project with its licence and credit recorded.

Find them under **File > Cloud Project…**, **File > Share Review Link…** (one dialog, two pages, plus a **Storage** page) and **Window > Stock Media**.
Both File commands are in the command palette (no default key).

## Where things are kept: Storage

**File > Cloud Project… > Storage > Add a location.** A location is a place you own:

| Kind | What it is | Good for |
|---|---|---|
| **Synced folder** | A folder inside OneDrive, Dropbox, Google Drive or a network share. Wabcut writes ordinary files there; the sync service moves them. | Cloud projects; review links you share with the service's own sharing |
| **WebDAV** | A WebDAV collection: Nextcloud, ownCloud, Synology, Apache or nginx WebDAV. Address, user and password. | Cloud projects; review links if the server shares them |
| **S3-compatible bucket** | AWS S3, Cloudflare R2, Backblaze B2, MinIO, Wasabi and the like. Endpoint, region, bucket, access key and secret key. | Cloud projects; review links (signed links, no public bucket) |

Every location is for **either** cloud projects **or** review links, never both: a review folder is shared with other people, and everything
in it is visible to them. Passwords and keys are saved in your system's key store (Windows Credential Manager, macOS Keychain, Secret
Service), never in a project, a setting or a log. **Test** checks that the storage can be reached and, for cloud projects, that it keeps its
promise about *conditional writes* (a write that says "only if nobody changed this since I read it"), which is what stops two editors from
overwriting each other. A storage that does not keep it cannot hold cloud projects (it can hold review links).

Wabcut refuses an unencrypted `http://` address (your password would travel in the open) unless it is this computer or you tick **Allow an
unencrypted connection**. A user name or password inside the address is refused; they are asked for separately.

**What you are trusting.** Whoever can write to a location can change the projects in it; whoever runs the storage can read everything in it.
Wabcut does not encrypt what it stores (a later version could). Use a storage you trust and give access only to people who should edit.

## Cloud projects

A cloud project is an ordinary `.wabcut` file on your computer that is linked to a copy on a storage. The media is **not** uploaded: each
computer keeps the media in its own place.

**Keep this project in the cloud.** On the *Cloud project* page choose a location, give it a name, read what goes up (the size of the project
file, and that it carries names, notes, transcripts and the paths of your media), tick the box and press **Upload as a cloud project**. Nothing
leaves the computer before that. Offline mode and the project's no-cloud lock switch the upload off, also for a folder (a sync service uploads it).

**Open it on another computer.** Add the same storage there, then *Look for projects*, **Open…**, and choose a folder. The project is downloaded,
opened, and Wabcut looks for its media in the project's folder and in the *media folders* you set for the location (**Media folder…** on the
Storage page). Media it cannot find stays offline until you relink it (the normal Link Media / Find media). A media path that points at a network
share of another computer (`\\server\share`) is never followed; the item is offline until you relink it.

**Sync now.** One button brings the two copies together:

* your changes only: sent as a new *revision*;
* the cloud's changes only: brought in as one undo step, **Sync Cloud Project**;
* both: merged with the project merge Wabcut already has (different sequences, tracks or clips merge; the same thing changed on both sides is a
  conflict), the result is sent;
* conflicts: nothing changes until you choose, per conflict, **Mine** or **the cloud's** (or *Keep all mine* / *Take all the cloud's*).

Revisions are never overwritten: each is a file named with its checksum, and only the small `head.json` says which is current, changed with a
compare-and-swap. If two computers save at the same moment, one loses the race, starts again from what the other saved and merges. A revision a
lost race left behind is listed as *not part of the current line* and can be opened as a copy. **Sync automatically after every Save** is an option,
off unless you turn it on.

If you undo the *Sync Cloud Project* step (or replace it with new edits), Wabcut does not send your project until you redo it or press **Send my
version anyway**: otherwise the changes of the other computer would be sent away as if you had deleted them.

**The editing lock.** While a cloud project is open, a small lock file on the storage says who is editing; Wabcut renews it every minute. If someone
else holds it, your project is **read-only** here (saving is off, nothing is sent); a lock that has not been renewed for 15 minutes is stale and is
taken over, with a message saying whose it was. The lock keeps careful editors apart; the compare-and-swap of `head.json` is what protects the
data. On a synced folder the lock is only as quick as the sync service, so two people who open the project within seconds may both think they have
it: their changes then merge when they sync. Keep the computers' clocks right: the age of a lock is judged by the clock of the one who looks.

**Revisions.** *Revisions in the cloud* lists the saved revisions (who, when, how big). **Open as a copy…** saves one as a separate project and opens
it. Each sync that changes your project also adds a checkpoint to the project's own version history.

**Stop using the cloud** removes the link; the copy on the storage stays where it is.

Limits of this version: media is not uploaded; no live co-editing; a project file may be up to 512 MB; one revision is not pruned from the storage
by Wabcut (delete old ones in the storage if you need the room); a pull that removes a redaction or changes an effect is merged like any other
change (the project merge shows what changed).

## Review links

**File > Share Review Link…** has three steps on one page.

1. **Make the review copy.** A small H.264 video of the sequence (540p, 720p or 1080p; the whole sequence, In to Out, or the work area), rendered on
   your computer with a progress bar and Cancel. Its size is shown before and after. Nothing is sent.
2. **Share it.** Choose a *review links* location, a title, how many days the link works, and a note for the reviewers. The page shows what will be
   uploaded and how big it is, and that anyone who gets the link can watch and comment; tick the box and press **Upload and publish**. Your storage
   provider bills for storage and traffic, not Wabcut. The video and a single web page (`index.html`) go into a folder named by 128 random bits.
3. **Read the comments back.** *Get comments* reads the files reviewers left in the link's `comments` folder; *Open a comments file…* reads one a
   reviewer sent you. They become **review notes** at the right frames (and, if you leave the box ticked, sequence markers), in one undo step,
   *Import Review Comments*. Nothing is added twice. The author reads "Maria (review link)": the name is whatever the reviewer typed.

**What the reviewer sees.** One page, no account: the video with a frame counter and timecode, left and right arrows (or the buttons) to step one frame,
a box for a name and a comment, and a click or drag on the picture to point at something. Comments stay in their browser until they press
**Download my comments**, which saves one small file to send back (or to drop into the shared folder's `comments` folder). The page makes no network
request of its own and runs no outside code; the comments it writes are plain text.

**How a link is hard to guess, expires and can be revoked, per kind of storage.**

| | Hard to guess | Expires | Revoke |
|---|---|---|---|
| **S3** | the folder name is 128 random bits; the page and video are signed links, no public bucket | by itself, after the days you chose (**7 days at most**; that is S3's limit) | **Revoke** deletes the files |
| **Synced folder** | the folder name is 128 random bits; you share that folder with the service's own sharing | the page refuses to play after the date; **Remove expired links** deletes the files | **Revoke** deletes the files (a copy someone already downloaded stays downloaded) |
| **WebDAV** | the same, and it depends on how the server shares the folder | as for a folder | as for a folder |

For a folder or WebDAV location the reviewer opens `index.html` from the shared folder (downloaded, or in the service's web view if it serves HTML):
the page works from a downloaded folder. Whether a service shows an HTML page in the browser or only offers it as a download is the service's
choice. Comments files carry a *public* link id (a hash of the folder name), never the secret folder name. A comments file can only add notes to the
sequence the link was made from; a file for another link is refused.

If the cut changes after you made the copy, the link row says so, and comment positions may sit a little off.

The Review panel's notes, replies and the markers made from them show reviewers' words as plain text only. In the imported text `<` and `>` become
the look-alike quotation marks `‹` `›`, so nothing a stranger writes can be read as markup by any panel.

**Command.** `review.importComments` imports one comments file through the command engine (`path` or `text`, the
page's public `link` id, where the copy `start`ed in the sequence, optional `duration`, `markers`). Uploading and downloading are not commands:
they happen only from the buttons.

## Stock media

**Window > Stock Media.** Choose **Pexels** or **Pixabay**, then **Videos** or **Photos**, type a search and press Enter.

* **Keys.** Both services give you a free key (the panel has *Get a free key*). Paste it under *Add key*: it is checked with a small search and
  saved in the key store. It is sent only to the service's API address.
* **Each result** shows its picture, who made it ("Video by Ann Lee on Pexels"), and the sizes it can be added in (1080p is chosen by default, not
  the 4K file). **Add to project** downloads the size you chose and imports it into a **Stock** bin in one undo step, **Add Stock Media**.
  The file goes to `<project>.wabcut-media/Stock` (or Documents/Wabcut/Stock for an unsaved project).
* **Licence and credit.** The item's rights record (File > Export > Rights and Clearance Log; the credits roll) is filled in: source (Pexels /
  Pixabay), licence (Pexels License / Pixabay Content License), the item's page, the credit line, status *Cleared*, and the date. Both services let
  you use the files free of charge, also commercially, without credit; they ask that results show where they come from (the panel always names the
  service and links its licence) and that the maker is credited where you can. They do not allow selling a file unchanged or showing identifiable
  people in a bad light. Read the licence the panel links before you rely on it; Wabcut records what each service says, it is not legal advice.
* **Offline mode** and **the project's no-cloud lock** switch searching and downloading off, with the reason. Thumbnails are fetched through the same
  network stack and kept in memory only; Pixabay forbids keeping its picture addresses, so none is stored. Searches are remembered for 24 hours
  (Pixabay requires it) so the same search again sends nothing.
* **Limits.** 200 searches an hour on Pexels, 100 a minute on Pixabay (a message says so). A download is limited to 4 GB, accepted only from the
  service's own hosts over https, checked to be the kind of file it says, and written to a temporary file that is renamed when complete.

## Privacy and security in short

* Nothing is uploaded without a button that says so, after the size is shown. Offline mode and the project's no-cloud lock stop every upload, also to
  a synced folder.
* Credentials live in the OS key store and in memory while Wabcut runs; they are never in a project, a setting, a log or an error message.
* Everything that comes from outside is checked: listings, `head.json`, locks, revisions (their checksum and the project loader's checks before they
  touch your project), comment files, and stock answers; names from a listing are never joined to a path before they are checked.
* A pull can switch the project's no-cloud lock **on**, never off.
* The review page's policy forbids every connection, every outside script and every inline script but its own (named by its hash); what reviewers
  and projects supply reaches it as data and is shown as text.

## Not in this version

Uploading media; real-time co-editing; multipart upload (files over 4.5 GB to S3); client-side encryption of what is stored; a Wabcut-hosted
service (none exists: storage is yours); a review page for comparing two versions; reviewer comments with replies on the page (comments are flat
and come back as new notes); pruning old revisions; checking for out-of-band changes of a folder by watching it (use **Sync now**).
