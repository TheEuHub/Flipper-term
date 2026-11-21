/* Browser-specific simulator. No native executable or hardware access.
 * Font and dolphin bitmap data are embedded in assets.js, derived from the
 * firmware sources as documented in README.md. */
(() => {
  'use strict';
  const screen = document.getElementById('screen');
  const ctx = screen.getContext('2d');
  if (!ctx) return;
  const led = document.getElementById('led');
  const pixels = new Uint8Array(128 * 64);
  const image = ctx.createImageData(128, 64);
  const buttons = [...document.querySelectorAll('[data-key]')];
  const storageKey = 'flipper-term-web-v1';
  const defaults = () => ({beep: false, timeout: 30, highscore: 0, saved: {}});
  function load() {
    try {
      const value = JSON.parse(localStorage.getItem(storageKey));
      if (!value || typeof value !== 'object') return defaults();
      return { ...defaults(), ...value, saved: value.saved && typeof value.saved === 'object' ? value.saved : {} };
    } catch (_) { return defaults(); }
  }
  let data = load();
  function persist() { try { localStorage.setItem(storageKey, JSON.stringify(data)); } catch (_) { /* private mode or quota */ } }
  let now = 0, lastFrame = 0, ledUntil = 0, lastInput = 0, audio = null;
  let state = { page: 'boot', started: 0 };
  const menu = ['Sub-GHz', '125 kHz RFID', 'NFC', 'Infrared', 'GPIO', 'iButton', 'Bad USB', 'Games', 'Settings'];
  const appMenus = {
    'Sub-GHz': ['Read', 'Read RAW', 'Saved', 'Add Manually', 'Frequency Analyzer', 'Region Information', 'Radio Settings'],
    '125 kHz RFID': ['Read', 'Saved', 'Add Manually', 'Extra Actions'],
    'NFC': ['Read', 'Read Specific Card', 'Saved', 'Add Manually', 'Extra Actions'],
    'Infrared': ['Universal Remotes', 'Learn New Remote', 'Saved Remotes', 'GPIO Settings'],
    'GPIO': ['USB-UART Bridge', 'USB-I2C', 'SPI Terminal', 'Pinout'],
    'iButton': ['Read', 'Saved', 'Add Manually'],
    'Bad USB': ['Scripts']
  };
  function saveId() {
    const bucket = Array.isArray(data.saved[state.app]) ? data.saved[state.app] : [];
    bucket.push({id: state.id, created: new Date().toISOString()});
    data.saved[state.app] = bucket.slice(-100); persist();
    state.page = 'message'; state.message = 'Saved in browser';
  }
  function setPixel(x, y, color = 1) { if (x >= 0 && x < 128 && y >= 0 && y < 64) pixels[(y | 0) * 128 + (x | 0)] = color; }
  function line(x0, y0, x1, y1, color = 1) {
    let dx = Math.abs(x1-x0), dy = -Math.abs(y1-y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx+dy;
    while (true) { setPixel(x0,y0,color); if (x0 === x1 && y0 === y1) break; const e = err*2; if (e >= dy) {err+=dy;x0+=sx;} if (e <= dx) {err+=dx;y0+=sy;} }
  }
  function rect(x,y,w,h,color=1) { for(let j=0;j<h;j++) for(let i=0;i<w;i++) setPixel(x+i,y+j,color); }
  function box(x,y,w,h,color=1) { line(x,y,x+w-1,y,color); line(x,y+h-1,x+w-1,y+h-1,color); line(x,y,x,y+h-1,color); line(x+w-1,y,x+w-1,y+h-1,color); }
  function width(text, font=1) { return [...String(text)].reduce((n,ch) => n + DEVICE_FONTS[font][ch.charCodeAt(0)>=32 && ch.charCodeAt(0)<=126 ? ch.charCodeAt(0)-32 : 0][12], 0); }
  function text(x,y,value,font=1,color=1,max=128) {
    let px=x;
    for(const ch of String(value)) {
      const n=ch.charCodeAt(0); const glyph=DEVICE_FONTS[font][n>=32 && n<=126?n-32:0];
      if(px+glyph[12]>x+max) break;
      for(let j=0;j<12;j++) for(let i=0;i<16;i++) if(glyph[j] & (1<<i)) setPixel(px+i,y+j-1,color);
      px+=glyph[12];
    }
  }
  function center(y,s,font=1) { text(Math.max(0,Math.floor((128-width(s,font))/2)),y,s,font); }
  function status() {
    rect(0,0,128,10); box(3,1,16,7,0); rect(19,3,2,3,0); rect(5,3,11,3,0);
    text(24,1,'87%',1,0); text(78,1,'B',1,0);
    const d=new Date(); const clock=String(d.getHours()).padStart(2,'0')+':'+String(d.getMinutes()).padStart(2,'0');
    text(126-width(clock),1,clock,1,0);
  }
  function header(title) { status(); text(4,12,title,0,1,120); line(0,21,127,21); }
  function footer(left,right) { line(0,54,127,54); text(4,56,left); text(124-width(right),56,right); }
  function drawMenu(title, items, selected) {
    header(title);
    if (!items.length) { center(32,'No files'); footer('Back',''); return; }
    let first=Math.max(0,selected-1); first=Math.min(first,Math.max(0,items.length-3));
    for(let i=first;i<Math.min(items.length,first+3);i++) { const y=23+(i-first)*10; if(i===selected) rect(1,y-1,125,9); text(4,y,items[i],1,i===selected?0:1,118); }
    footer('Back','OK');
  }
  function display() {
    const dim=data.timeout>0 && now-lastInput>data.timeout*1000;
    const paper=dim?[149,116,84]:[229,173,100], ink=[45,36,26];
    for(let i=0;i<pixels.length;i++) { const col=pixels[i]?ink:paper; image.data[i*4]=col[0]; image.data[i*4+1]=col[1]; image.data[i*4+2]=col[2]; image.data[i*4+3]=255; }
    ctx.putImageData(image,0,0); led.classList.toggle('active',now<ledUntil);
  }
  function drawDolphin() {
    const order=[0,1,2,3,2,3], frame=DOLPHIN_FRAMES[order[Math.floor(now/500)%order.length]];
    for(let y=0;y<50;y++) for(let x=0;x<128;x++) if(frame[y*16+(x>>3)] & (128>>(x&7))) setPixel(x,y+14);
  }
  function randId() { let value=''; for(let i=0;i<16;i++) value += Math.floor(Math.random()*16).toString(16); return value; }
  function select(key,count) { if(!count) return; if(key==='up') state.sel=(state.sel+count-1)%count; if(key==='down') state.sel=(state.sel+1)%count; }
  function openMain() { state={page:'main',sel:0}; }
  function openApp(app) {
    if(app==='Games') state={page:'games',sel:0};
    else if(app==='Settings') state={page:'settings',sel:0};
    else state={page:'app',app,sel:0};
  }
  function enterAppOption(option) {
    state.chosen=option; state.sel=0;
    if(option==='Saved' || option==='Saved Remotes' || option==='Scripts') state.page='files';
    else if(state.app==='Infrared' && option==='Universal Remotes') state.page='categories';
    else if(state.app==='GPIO') { state.page='gpio'; state.pins=Array(8).fill(false); }
    else if(['Read','Read RAW','Frequency Analyzer','Read Specific Card','Learn New Remote'].includes(option)) { state.page='reading'; state.id=randId(); }
    else { state.page='message'; state.message='Not simulated'; }
  }
  function playBeep() {
    if(!data.beep) return;
    try {
      const Audio=window.AudioContext || window.webkitAudioContext; if(!Audio) return;
      audio=audio || new Audio(); if(audio.state==='suspended') audio.resume();
      const oscillator=audio.createOscillator(), gain=audio.createGain();
      oscillator.frequency.value=700; gain.gain.setValueAtTime(0.035,audio.currentTime);
      gain.gain.exponentialRampToValueAtTime(0.001,audio.currentTime+0.07);
      oscillator.connect(gain); gain.connect(audio.destination); oscillator.start(); oscillator.stop(audio.currentTime+0.07);
    } catch (_) { /* Audio is optional. */ }
  }
  function snakeFood() {
    const free=[]; for(let y=0;y<6;y++) for(let x=0;x<16;x++) if(!state.snake.some(p=>p[0]===x&&p[1]===y)) free.push([x,y]);
    if(!free.length) {state.over=true;return;} state.food=free[Math.floor(Math.random()*free.length)];
  }
  function startSnake() { state={page:'snake',snake:[[3,3],[2,3],[1,3]],dx:1,dy:0,score:0,over:false,last:now}; snakeFood(); }
  function winner() {
    const b=state.board;
    for(const [a,c,d] of [[0,1,2],[3,4,5],[6,7,8],[0,3,6],[1,4,7],[2,5,8],[0,4,8],[2,4,6]]) if(b[a] && b[a]===b[c] && b[a]===b[d]) return b[a];
    return b.every(Boolean)?3:0;
  }
  function handle(key) {
    if(state.page==='boot') return;
    lastInput=now; ledUntil=now+180; playBeep();
    switch(state.page) {
      case 'desktop': if(key==='ok') openMain(); break;
      case 'main': select(key,menu.length); if(key==='back') state={page:'desktop'}; else if(key==='ok') openApp(menu[state.sel]); break;
      case 'app': { const list=appMenus[state.app]; select(key,list.length); if(key==='back') openMain(); else if(key==='ok') enterAppOption(list[state.sel]); break; }
      case 'files': {
        const list=Array.isArray(data.saved[state.app])?data.saved[state.app]:[]; select(key,list.length);
        if(key==='back') {state.page='app';state.sel=0;}
        else if(key==='ok' && list.length) {state.page='preview';state.item=list[state.sel];}
        break;
      }
      case 'preview': if(key==='back') state.page='files'; else if(key==='ok') {state.sentUntil=now+600; ledUntil=now+600;} break;
      case 'categories': { const categories=['TV','Audio','Air Conditioner','Projector','Fan']; select(key,categories.length); if(key==='back') {state.page='app';state.sel=0;} else if(key==='ok') {state.category=categories[state.sel];state.page='remote';state.sel=0;} break; }
      case 'remote': select(key,4); if(key==='back') {state.page='categories';state.sel=0;} else if(key==='ok') {state.sentUntil=now+600;ledUntil=now+600;} break;
      case 'reading': if(key==='back') {state.page='app';state.sel=0;} else if(key==='ok' && state.app!=='Sub-GHz') state.page='result'; break;
      case 'result': if(key==='back') {state.page='app';state.sel=0;} else if(key==='ok') saveId(); break;
      case 'gpio': if(key==='back') {state.page='app';state.sel=0;} else if(key==='left') state.sel=(state.sel+7)%8; else if(key==='right') state.sel=(state.sel+1)%8; else if(key==='ok') state.pins[state.sel]=!state.pins[state.sel]; break;
      case 'message': if(key==='back' || key==='ok') {state.page='app';state.sel=0;} break;
      case 'games': select(key,2); if(key==='back') openMain(); else if(key==='ok') {if(state.sel===0) startSnake(); else state={page:'ttt',board:Array(9).fill(0),cell:0};} break;
      case 'snake': if(key==='back') state={page:'games',sel:0}; else if(key==='up'&&state.dy!==1) {state.dx=0;state.dy=-1;} else if(key==='down'&&state.dy!==-1) {state.dx=0;state.dy=1;} else if(key==='left'&&state.dx!==1) {state.dx=-1;state.dy=0;} else if(key==='right'&&state.dx!==-1) {state.dx=1;state.dy=0;} break;
      case 'ttt': {
        if(key==='back') {state={page:'games',sel:1};break;}
        if(key==='left') state.cell=(state.cell+8)%9; if(key==='right') state.cell=(state.cell+1)%9;
        if(key==='up') state.cell=(state.cell+6)%9; if(key==='down') state.cell=(state.cell+3)%9;
        if(key==='ok'&&!winner()&&!state.board[state.cell]) { state.board[state.cell]=1; if(!winner()) { const free=state.board.map((v,i)=>v?-1:i).filter(i=>i>=0); state.board[free[Math.floor(Math.random()*free.length)]]=2; } } break;
      }
      case 'settings': select(key,3); if(key==='back') openMain(); else if(key==='ok'||key==='left'||key==='right') {
        if(state.sel===0) data.beep=!data.beep;
        else if(state.sel===1) data.timeout=data.timeout===0?15:data.timeout===15?30:data.timeout===30?60:0;
        else if(key==='ok') {state.page='confirm';break;} persist();
      } break;
      case 'confirm': if(key==='back') {state.page='settings';state.sel=2;} else if(key==='ok') {data=defaults();persist();state.page='resetDone';} break;
      case 'resetDone': if(key==='ok'||key==='back') state={page:'settings',sel:0}; break;
    }
  }
  function draw() {
    pixels.fill(0);
    switch(state.page) {
      case 'boot':
        center(20,'Flipper Zero',0); box(24,34,80,8); rect(25,35,Math.max(0,Math.min(78,Math.floor(now/1500*78))),6); center(46,'Loading...');
        if(now>1500) state={page:'desktop'}; break;
      case 'desktop': drawDolphin(); status(); break;
      case 'main': drawMenu('Main Menu',menu,state.sel); break;
      case 'app': drawMenu(state.app,appMenus[state.app],state.sel); break;
      case 'files': { const list=Array.isArray(data.saved[state.app])?data.saved[state.app]:[]; drawMenu(state.app==='Bad USB'?'Scripts':'Saved',list.map(x=>'SIM '+x.id),state.sel); break; }
      case 'preview': header(state.app); text(4,26,'Simulated ID:'); text(4,38,state.item.id); footer('Back','Sim send'); if(now<state.sentUntil) {rect(28,34,72,12);text(31,36,'SIMULATED',1,0);} break;
      case 'categories': drawMenu('Universal Remotes',['TV','Audio','Air Conditioner','Projector','Fan'],state.sel); break;
      case 'remote': drawMenu(state.category,['Power','Vol +','Vol -','Mute'],state.sel); if(now<state.sentUntil) {rect(29,34,70,12);text(32,36,'SIM SEND',1,0);} break;
      case 'reading':
        header(state.app);
        if(state.app==='Sub-GHz') { text(3,24,'433.920 MHz  SIM'); for(let x=4;x<124;x+=3){const h=1+((x*13+Math.floor(now/140)*7)%16);line(x,49,x,49-h);} footer('Back',''); }
        else {center(25,'SIMULATED READ');center(38,'OK to detect');footer('Back','OK');} break;
      case 'result': header(state.app); center(24,'Simulated ID:'); center(37,state.id); footer('Back','Save'); break;
      case 'message': header(state.app); center(32,state.message); footer('Back','OK'); break;
      case 'gpio':
        header(state.app);text(4,24,'SIMULATED PINOUT');for(let i=0;i<8;i++){const x=5+i*15;box(x,37,11,9);if(state.pins[i])rect(x+2,39,7,5);if(state.sel===i)line(x,49,x+10,49);}footer('< >','Toggle');break;
      case 'games': drawMenu('Games',['Snake','Tic-Tac-Toe'],state.sel); break;
      case 'snake': {
        if(!state.over && now-state.last>=180) {
          state.last=now; const head=[state.snake[0][0]+state.dx,state.snake[0][1]+state.dy];
          const grow=head[0]===state.food[0]&&head[1]===state.food[1];
          const body=grow?state.snake:state.snake.slice(0,-1);
          if(head[0]<0||head[0]>=16||head[1]<0||head[1]>=6||body.some(p=>p[0]===head[0]&&p[1]===head[1])) state.over=true;
          else {state.snake.unshift(head);if(!grow)state.snake.pop();else {state.score++;snakeFood();}}
          if(state.score>data.highscore){data.highscore=state.score;persist();}
        }
        text(2,1,'Snake '+state.score+' Best '+data.highscore);box(0,10,98,38);
        for(const [x,y] of state.snake) rect(2+x*6,11+y*6,5,5);box(2+state.food[0]*6,11+state.food[1]*6,5,5);
        text(2,50,state.over?'Game over - Back':'Arrows  Back:Exit'); break;
      }
      case 'ttt': {
        text(2,2,'Tic-Tac-Toe');for(let i=0;i<9;i++){const x=5+(i%3)*15,y=13+Math.floor(i/3)*15;box(x,y,14,14);if(i===state.cell)box(x+1,y+1,12,12);if(state.board[i])text(x+4,y+3,state.board[i]===1?'X':'O');}
        const w=winner(); text(56,25,w===1?'You win':w===2?'AI wins':w===3?'Draw':'Your turn');text(56,45,'Back');break;
      }
      case 'settings': drawMenu('Settings',['Beep: '+(data.beep?'On':'Off'),'Dim: '+(data.timeout?data.timeout+'s':'Never'),'Factory Reset'],state.sel); break;
      case 'confirm': header('Settings');center(27,'Erase browser data?');footer('Cancel','Erase');break;
      case 'resetDone': header('Settings');center(27,'Reset complete');footer('Back','OK');break;
    }
    display();
  }
  function tick(timestamp) {
    if(!lastFrame) lastFrame=timestamp;
    const delta=Math.max(0,Math.min(timestamp-lastFrame,100)); lastFrame=timestamp;
    if(!document.hidden) {now+=delta;draw();}
    requestAnimationFrame(tick);
  }
  const keyMap={ArrowUp:'up',ArrowDown:'down',ArrowLeft:'left',ArrowRight:'right',Enter:'ok',' ':'ok',Escape:'back',Backspace:'back'};
  document.addEventListener('keydown',event=>{
    const key=keyMap[event.key]; if(!key || event.ctrlKey || event.altKey || event.metaKey) return;
    const target = event.target;
    if (target && (target.isContentEditable || /^(A|INPUT|TEXTAREA|SELECT)$/.test(target.tagName))) return;
    // Let a focused control's native Enter/Space activation perform its own action.
    if (target && target.tagName === 'BUTTON' && (event.key === 'Enter' || event.key === ' ')) return;
    event.preventDefault(); const button=buttons.find(b=>b.dataset.key===key); if(button) button.classList.add('pressed'); handle(key);
  });
  document.addEventListener('keyup',event=>{const key=keyMap[event.key];if(key){const button=buttons.find(b=>b.dataset.key===key);if(button)button.classList.remove('pressed');}});
  window.addEventListener('blur',()=>buttons.forEach(b=>b.classList.remove('pressed')));
  document.addEventListener('visibilitychange',()=>{lastFrame=0;});
  buttons.forEach(button=>button.addEventListener('click',()=>handle(button.dataset.key)));
  requestAnimationFrame(tick);
})();
