#pragma once
// Browser chrome for full browser screens: tab strip, navigation row with the
// address bar, bookmarks bar, and (while "expanded" over the whole frame)
// bookmark menus and page dialogs. Chromium renders it as its own page next to
// the website; pvr-browser composites it above the page.
//
//   helper -> page: pvr.set(state), pvr.backdrop(dataUrl)
//   page -> helper: pvrSend(JSON) through a CDP binding
//
// Heights must match kTabsH / kNavH / kBookmarksH in pvr_browser.cpp.

namespace pvrbrowser {

inline const char* kToolbarHtml = R"PVRHTML(<!doctype html>
<html><head><meta charset="utf-8"><style>
:root{--bg:#161a22;--tab:#1f2430;--tabh:#272d3a;--act:#2c3341;--field:#0f1218;--text:#e8ecf3;
  --dim:#9aa4b5;--acc:#5aa2ff;--line:#2e3542}
body.inc{--bg:#1b1622;--tab:#251d31;--tabh:#2e243c;--act:#352a46;--field:#120e18;--acc:#b38cff;--line:#3a2f4a}
*{box-sizing:border-box}
html,body{margin:0;padding:0;background:transparent;color:var(--text);overflow:hidden;
  font:15px "Inter","Noto Sans","DejaVu Sans",sans-serif;user-select:none}
#bars{position:absolute;left:0;top:0;width:1280px;background:var(--bg);display:none}
body.bars #bars{display:block}
#tabs{height:34px;display:flex;align-items:flex-end;padding:0 6px;gap:2px}
.tab{flex:0 1 220px;min-width:44px;height:30px;display:flex;align-items:center;gap:6px;padding:0 6px 0 12px;
  background:var(--tab);border-radius:9px 9px 0 0;color:var(--dim);overflow:hidden}
.tab:hover{background:var(--tabh)}
.tab.a{background:var(--act);color:var(--text)}
.tab .t{flex:1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-size:13.5px}
.tab .x{flex:0 0 22px;height:22px;border-radius:6px;display:flex;align-items:center;justify-content:center;color:var(--dim)}
.tab .x:hover{background:#ffffff22;color:var(--text)}
.tab .dot{flex:0 0 8px;height:8px;border-radius:4px;background:var(--acc);animation:p 1s infinite alternate}
@keyframes p{from{opacity:.25}to{opacity:1}}
#newtab{flex:0 0 30px;height:28px;margin-left:4px;border-radius:8px;display:flex;align-items:center;justify-content:center;color:var(--dim)}
#newtab:hover{background:var(--tabh);color:var(--text)}
#nav{height:44px;display:flex;align-items:center;gap:4px;padding:0 8px;background:var(--act)}
.b{flex:0 0 36px;height:36px;border-radius:9px;display:flex;align-items:center;justify-content:center;color:var(--text)}
.b:hover{background:#ffffff1a}
.b.off{opacity:.35}
.b svg,.tab svg,#newtab svg{width:20px;height:20px;fill:none;stroke:currentColor;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
#addr{flex:1;height:34px;display:flex;align-items:center;background:var(--field);border-radius:17px;padding:0 6px 0 14px;
  border:1.5px solid transparent}
#addr.f{border-color:var(--acc)}
#addr input{flex:1;background:transparent;border:0;outline:0;color:var(--text);font:15px inherit;font-family:inherit;min-width:0}
#addr input::selection{background:var(--acc);color:#fff}
#star{flex:0 0 30px;height:30px}
#star.on svg{fill:#ffcc4d;stroke:#ffcc4d}
#badge{display:none;align-items:center;gap:6px;padding:0 10px;height:30px;border-radius:15px;background:#b38cff33;color:#d9c6ff;font-size:13px}
body.inc #badge{display:flex}
#dl{display:none;align-items:center;gap:6px;max-width:230px;padding:0 10px;height:30px;border-radius:15px;background:#ffffff14;font-size:13px}
#dl span{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
#bmbar{height:30px;display:none;align-items:center;gap:2px;padding:0 8px;background:var(--act);border-top:1px solid var(--line)}
body.hasbar #bmbar{display:flex}
.bm{flex:0 1 auto;max-width:170px;height:24px;display:flex;align-items:center;gap:6px;padding:0 9px;border-radius:7px;color:var(--text);font-size:13px}
.bm:hover{background:#ffffff1a}
.bm span{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.bm svg,.mi svg{flex:0 0 15px;width:15px;height:15px;fill:none;stroke:var(--dim);stroke-width:2}
#over{position:absolute;left:0;top:0;width:1280px;display:none}
body.exp #over{display:block}
#bd{position:absolute;left:0;width:1280px;height:720px;object-fit:fill}
#shade{position:absolute;left:0;right:0;bottom:0;background:#05070bb0}
.menu{position:absolute;width:420px;max-height:560px;overflow-y:auto;background:#1d222c;border:1px solid var(--line);
  border-radius:12px;padding:6px;box-shadow:0 12px 40px #000a}
.mi{height:34px;display:flex;align-items:center;gap:10px;padding:0 12px;border-radius:8px;font-size:14px;color:var(--text)}
.mi:hover{background:#ffffff14}
.mi.cur{background:#ffffff10;color:var(--acc)}
.mi span{flex:1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.mh{padding:8px 12px 4px;color:var(--dim);font-size:12px;text-transform:uppercase;letter-spacing:.06em}
.empty{padding:14px 12px;color:var(--dim);font-size:14px;line-height:1.45}
#dlg{position:absolute;left:340px;width:600px;background:#1d222c;border:1px solid var(--line);border-radius:14px;
  padding:22px 22px 18px;box-shadow:0 12px 40px #000a;display:none}
#dlg .m{font-size:16px;line-height:1.45;max-height:260px;overflow:auto;white-space:pre-wrap;word-break:break-word}
#dlg input{width:100%;margin-top:14px;height:38px;border-radius:9px;border:1.5px solid var(--line);background:var(--field);
  color:var(--text);padding:0 12px;font:15px inherit;font-family:inherit;outline:0}
#dlg input:focus{border-color:var(--acc)}
#dlg .r{display:flex;justify-content:flex-end;gap:10px;margin-top:18px}
.pb{height:38px;padding:0 20px;border-radius:9px;display:flex;align-items:center;background:#ffffff14;font-size:15px}
.pb:hover{background:#ffffff24}
.pb.p{background:var(--acc);color:#fff}
</style></head><body>
<div id="bars">
  <div id="tabs"></div>
  <div id="nav">
    <div class="b" id="back" title="Back"><svg viewBox="0 0 24 24"><path d="M15 5l-7 7 7 7"/></svg></div>
    <div class="b" id="fwd" title="Forward"><svg viewBox="0 0 24 24"><path d="M9 5l7 7-7 7"/></svg></div>
    <div class="b" id="reload" title="Reload"><svg viewBox="0 0 24 24"><path d="M20 11a8 8 0 1 0-2.3 5.7"/><path d="M20 4v7h-7"/></svg></div>
    <div class="b" id="home" title="Home"><svg viewBox="0 0 24 24"><path d="M4 11l8-7 8 7"/><path d="M6 10v10h12V10"/></svg></div>
    <div id="addr"><input id="url" spellcheck="false" autocomplete="off">
      <div class="b" id="star" title="Bookmark this page"><svg viewBox="0 0 24 24"><path d="M12 3.5l2.6 5.3 5.8.8-4.2 4.1 1 5.8-5.2-2.7-5.2 2.7 1-5.8-4.2-4.1 5.8-.8z"/></svg></div>
    </div>
    <div id="dl"><svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 4v11M7 10l5 5 5-5M5 20h14"/></svg><span></span></div>
    <div id="badge"><svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2"><path d="M3 11h18M6 11l2-6h8l2 6"/><circle cx="7.5" cy="16" r="2.5"/><circle cx="16.5" cy="16" r="2.5"/><path d="M10 16h4"/></svg>Incognito</div>
    <div class="b" id="bms" title="Bookmarks"><svg viewBox="0 0 24 24"><path d="M6 3h12v18l-6-4-6 4z"/></svg></div>
    <div class="b" id="full" title="Fullscreen (F11)"><svg viewBox="0 0 24 24"><path d="M4 9V4h5M15 4h5v5M20 15v5h-5M9 20H4v-5"/></svg></div>
  </div>
  <div id="bmbar"></div>
</div>
<div id="over"><img id="bd"><div id="shade"></div><div id="menu" class="menu" style="display:none"></div>
  <div id="dlg"><div class="m"></div><input><div class="r"><div class="pb" id="cancel">Cancel</div><div class="pb p" id="ok">OK</div></div></div>
</div>
<script>
const $=id=>document.getElementById(id);
const send=o=>{try{pvrSend(JSON.stringify(o))}catch(e){}};
const esc=s=>String(s||'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'})[c]);
const ICON_FOLDER='<svg viewBox="0 0 24 24"><path d="M3 6h7l2 2h9v11H3z"/></svg>';
const ICON_PAGE='<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3c3 3 3 15 0 18M12 3c-3 3-3 15 0 18"/></svg>';
let S={tabs:[],a:0,url:'',bm:{bar:[],other:[]},bars:true};
let editing=false;
function toUrl(t){t=t.trim();if(!t)return '';
  if(/^[a-z][a-z0-9+.-]*:/i.test(t)&&!/^[^\s:]+:\d/.test(t)&&t.indexOf(' ')<0)return t;
  if(t.indexOf(' ')<0&&(t.indexOf('.')>0||/^localhost(:|\/|$)/.test(t)||/^[^\s:]+:\d+/.test(t)))return 'https://'+t;
  return 'https://www.google.com/search?q='+encodeURIComponent(t);}
function renderTabs(){
  let h='';
  S.tabs.forEach((t,i)=>{h+='<div class="tab'+(i==S.a?' a':'')+'" data-i="'+i+'">'+(t.l?'<div class="dot"></div>':'')+
    '<div class="t">'+esc(t.t||t.u||'New Tab')+'</div><div class="x" data-x="'+i+'"><svg viewBox="0 0 24 24"><path d="M6 6l12 12M18 6L6 18"/></svg></div></div>';});
  h+='<div id="newtab" title="New tab"><svg viewBox="0 0 24 24"><path d="M12 5v14M5 12h14"/></svg></div>';
  $('tabs').innerHTML=h;
}
function nodeHtml(n,depth){
  if(n.c){let h='<div class="mi" style="padding-left:'+(12+depth*18)+'px">'+ICON_FOLDER+'<span>'+esc(n.n)+'</span></div>';
    n.c.forEach(c=>h+=nodeHtml(c,depth+1));return h;}
  return '<div class="mi" data-u="'+esc(n.u)+'" style="padding-left:'+(12+depth*18)+'px">'+ICON_PAGE+'<span>'+esc(n.n||n.u)+'</span></div>';
}
function renderBar(){
  let h='';
  (S.bm.bar||[]).forEach((n,i)=>{h+='<div class="bm" '+(n.c?'data-f="'+i+'"':'data-u="'+esc(n.u)+'"')+'>'+(n.c?ICON_FOLDER:ICON_PAGE)+'<span>'+esc(n.n||n.u)+'</span></div>';});
  $('bmbar').innerHTML=h;
}
let menuOpen=null;
function openMenu(kind,x){
  menuOpen=kind;const m=$('menu');let h='';
  if(kind==='all'){
    const bar=S.bm.bar||[],oth=S.bm.other||[];
    if(!bar.length&&!oth.length)h='<div class="empty">No bookmarks yet. Use the star in the address bar to save the current page.</div>';
    if(bar.length){h+='<div class="mh">Bookmarks bar</div>';bar.forEach(n=>h+=nodeHtml(n,0));}
    if(oth.length){h+='<div class="mh">Other bookmarks</div>';oth.forEach(n=>h+=nodeHtml(n,0));}
  }else{const f=(S.bm.bar||[])[kind];if(f&&f.c){f.c.forEach(n=>h+=nodeHtml(n,0));if(!f.c.length)h='<div class="empty">Empty folder</div>';}}
  m.innerHTML=h;m.style.display='block';
  const top=(S.bars?barsH():0)+4;m.style.top=top+'px';
  m.style.maxHeight=(Math.max(200,(S.fh||800)-top-16))+'px';
  m.style.left=Math.max(8,Math.min(1280-428,x))+'px';
  send({t:'menu',on:true});
}
// A page's <select>, opened as a menu over the page at the element.
function openSelect(sel){
  menuOpen='sel';const m=$('menu');let h='',grp=null;
  sel.o.forEach((o,i)=>{
    if(o[2]&&o[2]!==grp){grp=o[2];h+='<div class="mh">'+esc(grp)+'</div>';}
    h+='<div class="mi'+(i===sel.i?' cur':'')+'"'+(o[1]?' style="opacity:.4"':' data-p="'+i+'"')+'><span>'+esc(o[0])+'</span></div>';
  });
  m.innerHTML=h||'<div class="empty">No options</div>';m.style.display='block';
  const top=S.bars?barsH():0,fh=S.fh||720;
  m.style.width=Math.max(220,Math.min(600,sel.w+24))+'px';
  m.style.left=Math.max(8,Math.min(1280-m.offsetWidth-8,sel.x))+'px';
  const below=fh-(top+sel.y+sel.h)-12,above=top+sel.y-12;
  const want=Math.min(m.scrollHeight,560);
  if(below>=want||below>=above){m.style.maxHeight=Math.max(120,below)+'px';m.style.top=(top+sel.y+sel.h+2)+'px';}
  else{const hh=Math.min(want,above);m.style.maxHeight=hh+'px';m.style.top=(top+sel.y-hh-2)+'px';}
  const c=m.querySelector('.cur');if(c)c.scrollIntoView({block:'center'});
}
function closeMenu(){if(menuOpen===null)return;menuOpen=null;$('menu').style.display='none';if(!S.dlg)send({t:'menu',on:false});}
function barsH(){return 78+((S.bm.bar||[]).length?30:0);}
function renderDialog(){
  const d=S.dlg,el=$('dlg');
  if(!d){el.style.display='none';return;}
  el.querySelector('.m').textContent=d.m||(d.k==='beforeunload'?'Leave this page? Changes you made may not be saved.':'');
  const inp=el.querySelector('input');inp.style.display=d.k==='prompt'?'block':'none';
  if(el.style.display!=='block'){inp.value=d.d||'';}
  $('cancel').style.display=d.k==='alert'?'none':'flex';
  $('ok').textContent=d.k==='beforeunload'?'Leave':'OK';
  el.style.display='block';el.style.top=Math.max(40,((S.fh||720)-el.offsetHeight)/2)+'px';
}
window.pvr={
  set(s){
    S=s;document.body.classList.toggle('inc',!!s.inc);document.body.classList.toggle('bars',!!s.bars);
    document.body.classList.toggle('hasbar',!!(s.bm.bar||[]).length);document.body.classList.toggle('exp',!!s.exp);
    renderTabs();renderBar();
    if(!editing)$('url').value=s.url||'';
    $('back').classList.toggle('off',!s.back);$('fwd').classList.toggle('off',!s.fwd);
    $('reload').innerHTML=s.loading?'<svg viewBox="0 0 24 24"><path d="M6 6l12 12M18 6L6 18"/></svg>':'<svg viewBox="0 0 24 24"><path d="M20 11a8 8 0 1 0-2.3 5.7"/><path d="M20 4v7h-7"/></svg>';
    $('star').classList.toggle('on',!!s.starred);
    const dl=$('dl');if(s.dl){dl.style.display='flex';dl.querySelector('span').textContent=s.dl;}else dl.style.display='none';
    $('bd').style.top=(s.bars?barsH():0)+'px';$('shade').style.top=(s.bars?barsH():0)+'px';
    $('over').style.height=(s.fh||720)+'px';
    if(!s.exp&&menuOpen!==null){menuOpen=null;$('menu').style.display='none';}
    if(s.sel&&s.exp&&menuOpen!=='sel')openSelect(s.sel);
    renderDialog();
  },
  backdrop(u){$('bd').src=u;}
};
document.addEventListener('click',e=>{
  const t=e.target.closest('[data-x],[data-i],[data-p],[data-u],[data-f],.b,#newtab,.pb,#menu,#dlg,#shade,#over,#addr');
  if(!t||t.id==='menu'||t.id==='dlg')return;
  if(t.dataset.x!==undefined){send({t:'close',i:+t.dataset.x});return;}
  if(t.dataset.i!==undefined){send({t:'tab',i:+t.dataset.i});return;}
  if(t.dataset.p!==undefined){menuOpen=null;$('menu').style.display='none';send({t:'pick',i:+t.dataset.p});return;}
  if(t.dataset.u!==undefined){closeMenu();send({t:'nav',u:t.dataset.u});return;}
  if(t.dataset.f!==undefined){const r=t.getBoundingClientRect();if(menuOpen===+t.dataset.f)closeMenu();else openMenu(+t.dataset.f,r.left);return;}
  switch(t.id){
    case 'newtab':send({t:'new'});break;
    case 'back':send({t:'back'});break;
    case 'fwd':send({t:'fwd'});break;
    case 'reload':send({t:S.loading?'stop':'reload'});break;
    case 'home':send({t:'home'});break;
    case 'star':send({t:'star'});break;
    case 'full':send({t:'full'});break;
    case 'bms':if(menuOpen==='all')closeMenu();else openMenu('all',1280-436);break;
    case 'ok':answer(true);break;
    case 'cancel':answer(false);break;
    case 'shade':case 'over':if(!S.dlg)closeMenu();break;
    case 'addr':$('url').focus();break;
  }
});
function answer(ok){send({t:'dialog',ok:ok,v:$('dlg').querySelector('input').value});}
const url=$('url');
url.addEventListener('focus',()=>{editing=true;$('addr').classList.add('f');setTimeout(()=>url.select(),0);});
url.addEventListener('blur',()=>{editing=false;$('addr').classList.remove('f');url.value=S.url||'';send({t:'blur'});});
url.addEventListener('keydown',e=>{
  if(e.key==='Enter'){const u=toUrl(url.value);if(u)send({t:'nav',u:u});url.blur();}
  else if(e.key==='Escape'){url.blur();}
});
$('dlg').querySelector('input').addEventListener('keydown',e=>{if(e.key==='Enter')answer(true);});
</script></body></html>)PVRHTML";

}  // namespace pvrbrowser
