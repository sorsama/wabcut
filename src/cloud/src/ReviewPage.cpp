// The static review page (ReviewLink.h): one HTML file, no external request, comments leave as a file.
//
// Everything that varies per link (title, id, frame rate, expiry, the video address, the editor's note) is JSON in a data block
// the browser does not run; the script and the style are the same for every page, so their hashes name them in the CSP.

#include <wabcut/cloud/ReviewLink.h>

#include <wabcut/ai/Http.h>
#include <wabcut/ai/Sha256.h>
#include <wabcut/model/ReviewComments.h>

#include <nlohmann/json.hpp>

namespace wabcut::cloud {

namespace {

std::string htmlText(std::string_view text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&#39;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

/// JSON that can sit inside <script type="application/json">: nothing in it can close the element or start a comment.
std::string safeJson(const nlohmann::json& j) {
    const std::string raw = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '<') {
            out += "\\u003c";
        } else if (c == '>') {
            out += "\\u003e";
        } else if (c == '&') {
            out += "\\u0026";
        } else if (c == '\xE2' && i + 2 < raw.size() && raw[i + 1] == '\x80' && (raw[i + 2] == '\xA8' || raw[i + 2] == '\xA9')) {
            out += raw[i + 2] == '\xA8' ? "\\u2028" : "\\u2029";
            i += 2;
        } else {
            out += c;
        }
    }
    return out;
}

std::string hashOf(std::string_view text) {
    ai::Sha256 h;
    h.update(text);
    const auto digest = h.finish();
    return "sha256-" + base64Encode(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

// MSVC limits one string literal piece to about 16 KB: the style and the script are written in pieces.
const std::string& styleText() {
    static const std::string css = std::string(
        R"CSS(
:root{color-scheme:light dark;--bg:#f6f6f8;--card:#fff;--text:#1d1d22;--muted:#5d5d6a;--line:#d6d6e0;--accent:#7c3aed;--accent-text:#fff;--mark:#ffcc00}
@media (prefers-color-scheme:dark){:root{--bg:#141418;--card:#1f1f25;--text:#ececf1;--muted:#a3a3b2;--line:#33333d;--accent:#a78bfa;--accent-text:#111}}
*{box-sizing:border-box}
[hidden]{display:none!important}
body{margin:0;padding:20px 16px 40px;background:var(--bg);color:var(--text);font:15px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:980px;margin:0 auto}
h1{font-size:22px;margin:0 0 2px}h2{font-size:17px;margin:24px 0 8px}
.muted{color:var(--muted);margin:2px 0 10px}
#stage{position:relative;background:#000;border-radius:10px;overflow:hidden;line-height:0}
#stage video{width:100%;max-height:70vh;background:#000}
#overlay{position:absolute;left:0;top:0;right:0;bottom:56px;cursor:crosshair;touch-action:none}
.mk{position:absolute;border:2px solid var(--mark);border-radius:3px;pointer-events:none}
.mk.dot{width:16px;height:16px;margin:-8px 0 0 -8px;border-radius:50%}
.mk.pending{border-style:dashed}
#bar{display:flex;flex-wrap:wrap;align-items:center;gap:10px;margin:10px 0}
#tc{font:600 18px ui-monospace,Consolas,monospace}
button{font:inherit;padding:6px 14px;border-radius:999px;border:1px solid var(--line);background:var(--card);color:var(--text);cursor:pointer}
button:hover{border-color:var(--accent)}
button:focus-visible,input:focus-visible,textarea:focus-visible{outline:2px solid var(--accent);outline-offset:2px}
button.primary{background:var(--accent);color:var(--accent-text);border-color:var(--accent);font-weight:600}
label{display:block;margin:10px 0 4px;font-weight:600}
input,textarea{width:100%;font:inherit;padding:8px 10px;border-radius:8px;border:1px solid var(--line);background:var(--card);color:var(--text)}
ol{list-style:none;margin:0;padding:0}
li{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:10px 12px;margin:0 0 8px;display:flex;gap:10px;flex-wrap:wrap;align-items:flex-start}
li .body{flex:1 1 240px;min-width:0;white-space:pre-wrap;overflow-wrap:anywhere}
li .time{font:600 14px ui-monospace,Consolas,monospace}
#status{min-height:1.4em}
)CSS");
    return css;
}

const std::string& scriptText() {
    static const std::string js = std::string(R"JS(
'use strict';
(function () {
  var M;
  try { M = JSON.parse(document.getElementById('manifest').textContent); } catch (e) { return; }
  function $(id) { return document.getElementById(id); }
  $('title').textContent = M.title;
  var fps = M.fpsNum / M.fpsDen;
  var nominal = Math.round(fps);
  var lastFrame = M.durationFrames > 0 ? M.durationFrames - 1 : 1e8;
  var v = $('v'), overlay = $('overlay'), stage = $('stage');
  var comments = [], pending = null, frame = 0, dragging = null;
  var storeKey = 'wabreview-' + M.linkId;

  if (M.expiresAt && Date.now() / 1000 > M.expiresAt) {
    $('expired').hidden = false;
    $('player').hidden = true; $('compose').hidden = true; $('listing').hidden = true;
    return;
  }
  if (M.expiresAt) {
    $('expiry').textContent = 'This review link works until ' + new Date(M.expiresAt * 1000).toLocaleDateString() + '.';
  }
  if (M.instructions) { $('instructions').textContent = M.instructions; }
  v.src = M.video;

  function pad(n, w) { var s = String(n); while (s.length < w) { s = '0' + s; } return s; }
  function timecode(f) {
    var drop = M.dropFrame && (nominal === 30 || nominal === 60);
    if (drop) {
      var d = Math.round(fps * 0.066666), perMin = Math.round(fps * 60), per10 = Math.round(fps * 600);
      var tens = Math.floor(f / per10), rest = f % per10;
      f += d * 9 * tens + (rest > d ? d * Math.floor((rest - d) / perMin) : 0);
    }
    var ff = f % nominal, s = Math.floor(f / nominal);
    return pad(Math.floor(s / 3600), 2) + ':' + pad(Math.floor(s / 60) % 60, 2) + ':' + pad(s % 60, 2) + (drop ? ';' : ':') + pad(ff, 2);
  }
  function rand(bytes) {
    var out = '';
    try { var a = new Uint8Array(bytes); crypto.getRandomValues(a); for (var i = 0; i < a.length; i++) { out += pad(a[i].toString(16), 2); } }
    catch (e) { for (var j = 0; j < bytes; j++) { out += pad(Math.floor(Math.random() * 256).toString(16), 2); } }
    return out;
  }
  function say(text) { $('status').textContent = text; }

  function show() {
    $('tc').textContent = timecode(frame);
    $('addtc').textContent = timecode(frame);
    var marks = stage.querySelectorAll('.mk.shown');
    for (var i = 0; i < marks.length; i++) { marks[i].remove(); }
    for (var k = 0; k < comments.length; k++) { if (comments[k].frame === frame && comments[k].mark) { drawMark(comments[k].mark, false); } }
    if (pending) { drawMark(pending, true); }
  }
  // where the picture really is inside the video box (it is letterboxed to keep its shape)
  function picture() {
    var bw = v.clientWidth, bh = v.clientHeight;
    if (!v.videoWidth || !v.videoHeight || !bw || !bh) { return { left: 0, top: 0, width: Math.max(1, bw), height: Math.max(1, bh) }; }
    var k = Math.min(bw / v.videoWidth, bh / v.videoHeight), w = v.videoWidth * k, h = v.videoHeight * k;
    return { left: (bw - w) / 2, top: (bh - h) / 2, width: w, height: h };
  }
  function drawMark(m, isPending) {
    var p = picture(), dot = m.width < 0.01 && m.height < 0.01;
    var el = document.createElement('div');
    el.className = 'mk shown' + (dot ? ' dot' : '') + (isPending ? ' pending' : '');
    el.style.left = (p.left + m.x * p.width) + 'px'; el.style.top = (p.top + m.y * p.height) + 'px';
    if (!dot) { el.style.width = (m.width * p.width) + 'px'; el.style.height = (m.height * p.height) + 'px'; }
    stage.appendChild(el);
  }
  window.addEventListener('resize', show);
  v.addEventListener('loadedmetadata', show);

  if (typeof v.requestVideoFrameCallback === 'function') {
    var onFrame = function (now, meta) { frame = Math.max(0, Math.round(meta.mediaTime * fps)); show(); v.requestVideoFrameCallback(onFrame); };
    v.requestVideoFrameCallback(onFrame);
  } else {
    var follow = function () { frame = Math.max(0, Math.floor(v.currentTime * fps + 1e-3)); show(); };
    v.addEventListener('timeupdate', follow); v.addEventListener('seeked', follow);
  }
  function seekTo(f) {
    f = Math.max(0, Math.min(lastFrame, f)); v.pause(); v.currentTime = (f + 0.5) / fps; frame = f; show();
  }
  $('prev').addEventListener('click', function () { seekTo(frame - 1); });
  $('next').addEventListener('click', function () { seekTo(frame + 1); });
  document.addEventListener('keydown', function (e) {
    var t = e.target && e.target.tagName;
    if (t === 'INPUT' || t === 'TEXTAREA' || e.ctrlKey || e.metaKey || e.altKey) { return; }
    if (e.key === 'ArrowLeft' || e.key === ',') { e.preventDefault(); seekTo(frame - 1); }
    else if (e.key === 'ArrowRight' || e.key === '.') { e.preventDefault(); seekTo(frame + 1); }
  });

  // pointing at the picture: a click is a point, a drag a box (fractions of the picture)
  function at(e) {
    var r = overlay.getBoundingClientRect(), p = picture();
    return { x: Math.min(1, Math.max(0, (e.clientX - r.left - p.left) / p.width)), y: Math.min(1, Math.max(0, (e.clientY - r.top - p.top) / p.height)) };
  }
  overlay.addEventListener('pointerdown', function (e) {
    v.pause(); dragging = at(e); try { overlay.setPointerCapture(e.pointerId); } catch (x) { /* not needed */ }
  });
  overlay.addEventListener('pointermove', function (e) {
    if (!dragging) { return; }
    var p = at(e);
    pending = { x: Math.min(dragging.x, p.x), y: Math.min(dragging.y, p.y), width: Math.abs(p.x - dragging.x), height: Math.abs(p.y - dragging.y) };
    show();
  });
  overlay.addEventListener('pointerup', function (e) {
    if (!dragging) { return; }
    var p = at(e);
    var w = Math.abs(p.x - dragging.x), h = Math.abs(p.y - dragging.y);
    pending = (w < 0.01 && h < 0.01) ? { x: p.x, y: p.y, width: 0, height: 0 } : { x: Math.min(dragging.x, p.x), y: Math.min(dragging.y, p.y), width: w, height: h };
    dragging = null; $('clearmark').hidden = false; show();
  });
  $('clearmark').addEventListener('click', function () { pending = null; $('clearmark').hidden = true; show(); });

  function render() {
    var list = $('list');
    while (list.firstChild) { list.removeChild(list.firstChild); }
    comments.forEach(function (c) {
      var li = document.createElement('li');
      var go = document.createElement('button'); go.type = 'button'; go.className = 'time'; go.textContent = timecode(c.frame);
      go.addEventListener('click', function () { seekTo(c.frame); });
      var body = document.createElement('div'); body.className = 'body'; body.textContent = c.text;
      var del = document.createElement('button'); del.type = 'button'; del.textContent = 'Delete';
      del.addEventListener('click', function () { comments = comments.filter(function (x) { return x !== c; }); render(); persist(); show(); });
      li.appendChild(go); li.appendChild(body); li.appendChild(del); list.appendChild(li);
    });
    $('count').textContent = String(comments.length);
    $('save').disabled = comments.length === 0;
  }
  function persist() {
    try { localStorage.setItem(storeKey, JSON.stringify({ name: $('name').value, comments: comments })); } catch (e) { /* storage may be blocked */ }
  }
  function unit(n) { return typeof n === 'number' && n >= 0 && n <= 1; }
  function restore() {
    try {
      var d = JSON.parse(localStorage.getItem(storeKey) || 'null');
      if (!d || !Array.isArray(d.comments)) { return; }
      if (typeof d.name === 'string') { $('name').value = d.name.slice(0, 100); }
      d.comments.slice(0, 2000).forEach(function (c) {
        if (c && typeof c.text === 'string' && c.text.length <= 4000 && Number.isInteger(c.frame) && c.frame >= 0 && c.frame <= 1e8 && typeof c.id === 'string' && c.id.length <= 40) {
          var m = c.mark && unit(c.mark.x) && unit(c.mark.y) && unit(c.mark.width) && unit(c.mark.height) ? c.mark : null;
          comments.push({ id: c.id, frame: c.frame, text: c.text, mark: m, createdAt: typeof c.createdAt === 'string' ? c.createdAt.slice(0, 32) : '' });
        }
      });
    } catch (e) { /* nothing saved, or unreadable: start empty */ }
  }

  $('add').addEventListener('click', function () {
    var text = $('text').value.trim();
    if (!text) { say('Write a comment first.'); $('text').focus(); return; }
    if (comments.length >= 2000) { say('That is the most comments one page keeps. Download them, then start a new page.'); return; }
    comments.push({ id: rand(8), frame: frame, text: text.slice(0, 4000), mark: pending, createdAt: new Date().toISOString().replace(/\.\d+Z$/, 'Z') });
    comments.sort(function (a, b) { return a.frame - b.frame || (a.createdAt < b.createdAt ? -1 : 1); });
    pending = null; $('clearmark').hidden = true; $('text').value = '';
    render(); persist(); show(); say('Comment added at ' + timecode(frame) + '. Download your comments when you are done.');
  });
  $('text').addEventListener('keydown', function (e) { if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) { e.preventDefault(); $('add').click(); } });
  $('name').addEventListener('change', persist);

  $('save').addEventListener('click', function () {
    var name = ($('name').value.trim() || 'Reviewer').slice(0, 100);
    var data = { format: 'wabcut-review-comments', version: 1, link: M.linkId, reviewer: name,
      comments: comments.map(function (c) { return { frame: c.frame, text: c.text, mark: c.mark || undefined, createdAt: c.createdAt }; }) };
    var slug = name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 24) || 'reviewer';
    var a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' }));
    a.download = 'comments-' + slug + '-' + rand(2) + '.json';
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(function () { URL.revokeObjectURL(a.href); }, 10000);
    say('Saved ' + a.download + '. Send that file back to the person who shared this page' + (M.share ? ' or put it in the "comments" folder.' : '.'));
  });

  restore(); render(); show();
})();
)JS");
    return js;
}

} // namespace

std::string publicLinkId(std::string_view token) { return sha256Hex(token).substr(0, 24); }

ReviewPage buildReviewPage(const ReviewPageInput& in) {
    ReviewPage page;
    const std::string& css = styleText();
    const std::string& js = scriptText();
    page.styleHash = hashOf(css);
    page.scriptHash = hashOf(js);

    std::string mediaSrc = "'self' blob:";
    if (in.fromFiles) {
        mediaSrc += " file:";
    }
    if (const auto url = ai::parseHttpUrl(in.videoUrl)) {
        mediaSrc += " " + url->scheme + "://" + url->host + (url->port ? ":" + std::to_string(*url->port) : std::string());
    }
    const std::string csp = "default-src 'none'; script-src '" + page.scriptHash + "'; style-src '" + page.styleHash + "'; media-src " + mediaSrc +
                            "; img-src data:; connect-src 'none'; form-action 'none'; base-uri 'none'; require-trusted-types-for 'script'; "
                            "trusted-types 'none'";

    const std::int64_t num = in.frameRate.isValid() ? in.frameRate.num : 30;
    const std::int64_t den = in.frameRate.isValid() ? in.frameRate.den : 1;
    nlohmann::json manifest{{"v", 1},
                            {"linkId", in.linkId},
                            {"title", in.title},
                            {"video", in.videoUrl},
                            {"fpsNum", num},
                            {"fpsDen", den},
                            {"dropFrame", in.dropFrame},
                            {"durationFrames", in.durationFrames},
                            {"expiresAt", in.expiresAt},
                            {"instructions", in.instructions},
                            {"share", in.fromFiles}};

    page.html.reserve(css.size() + js.size() + 3000);
    page.html += "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n";
    page.html += "<meta http-equiv=\"Content-Security-Policy\" content=\"" + htmlText(csp) + "\">\n";
    page.html += "<meta name=\"referrer\" content=\"no-referrer\">\n";
    page.html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    page.html += "<meta name=\"robots\" content=\"noindex, nofollow\">\n";
    page.html += "<title>" + htmlText(in.title.empty() ? std::string("Review") : in.title) + "</title>\n";
    page.html += "<style>" + css + "</style></head>\n";
    page.html +=
        "<body><main>\n"
        "<header><h1 id=\"title\"></h1><p id=\"expiry\" class=\"muted\"></p></header>\n"
        "<p id=\"expired\" hidden><strong>This review link has expired.</strong> Ask the person who sent it for a new one.</p>\n"
        "<section id=\"player\" aria-label=\"Video\">\n"
        "<div id=\"stage\"><video id=\"v\" controls preload=\"metadata\" playsinline aria-label=\"Review video\"></video>"
        "<div id=\"overlay\" title=\"Click to point at something, drag to draw a box\"></div></div>\n"
        "<div id=\"bar\"><button id=\"prev\" type=\"button\" aria-label=\"Previous frame\">&#9664; Frame</button>"
        "<button id=\"next\" type=\"button\" aria-label=\"Next frame\">Frame &#9654;</button>"
        "<output id=\"tc\" aria-label=\"Timecode\">00:00:00:00</output></div>\n"
        "</section>\n"
        "<section id=\"compose\">\n"
        "<p id=\"instructions\" class=\"muted\"></p>\n"
        "<label for=\"name\">Your name</label><input id=\"name\" maxlength=\"100\" autocomplete=\"name\">\n"
        "<label for=\"text\">Comment</label><textarea id=\"text\" rows=\"3\" maxlength=\"4000\"></textarea>\n"
        "<p class=\"muted\">Pause on the frame you mean, then add your comment. Click on the picture to point at something "
        "(drag to draw a box). Left and right arrow keys step one frame.</p>\n"
        "<p><button id=\"add\" class=\"primary\" type=\"button\">Add comment at <span id=\"addtc\">00:00:00:00</span></button> "
        "<button id=\"clearmark\" type=\"button\" hidden>Remove the mark</button></p>\n"
        "<p id=\"status\" class=\"muted\" role=\"status\" aria-live=\"polite\"></p>\n"
        "</section>\n"
        "<section id=\"listing\"><h2>Your comments (<span id=\"count\">0</span>)</h2><ol id=\"list\"></ol>\n"
        "<p><button id=\"save\" type=\"button\" disabled>Download my comments</button></p>\n"
        "<p class=\"muted\">Your comments stay in this browser until you download them. The download is one small file; send it back to the "
        "person who shared this page.</p></section>\n"
        "</main>\n";
    page.html += "<script type=\"application/json\" id=\"manifest\">" + safeJson(manifest) + "</script>\n";
    page.html += "<script>" + js + "</script>\n</body></html>\n";
    return page;
}

} // namespace wabcut::cloud
