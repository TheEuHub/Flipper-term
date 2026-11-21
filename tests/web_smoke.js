// Node-only DOM/canvas harness: verifies navigation, drawing and browser storage.
const assert = require('assert');
const fs = require('fs');
const vm = require('vm');
let nextFrame, stamp = 0, frame = null;
const listeners = {}, values = new Map();
const buttons = ['up','down','left','right','ok','back'].map(key => ({
  dataset: {key}, classList: {add(){},remove(){},toggle(){}},
  addEventListener(type, handler) {this[type] = handler;}
}));
const ctx = {createImageData: (w,h) => ({data:new Uint8ClampedArray(w*h*4)}), putImageData(image) {frame=Uint8Array.from(image.data);}};
const screen = {getContext: () => ctx}, led = {classList: {toggle(){}}};
const document = {hidden:false, getElementById(id){return id==='screen'?screen:led;},
  querySelectorAll(){return buttons;}, addEventListener(type, cb){listeners[type]=cb;}};
const sandbox = {document, window:{addEventListener(){}}, localStorage:{getItem:k=>values.get(k) || null,setItem:(k,v)=>values.set(k,v)},
  requestAnimationFrame: cb=>{nextFrame=cb;}, Uint8Array, Uint8ClampedArray, Date, Math, JSON, console};
vm.createContext(sandbox);
vm.runInContext(fs.readFileSync('web/assets.js','utf8'),sandbox);
vm.runInContext(fs.readFileSync('web/app.js','utf8'),sandbox);
function tick(n=1){for(let i=0;i<n;i++){stamp+=50;const fn=nextFrame;fn(stamp);}}
function click(key){buttons.find(b=>b.dataset.key===key).click();tick();}
function snapshot(){return Buffer.from(frame).toString('base64');}
tick(35); const desktop=snapshot();
click('ok'); const main=snapshot(); assert.notStrictEqual(desktop,main);
click('down'); click('ok'); // RFID
click('ok'); // Read
click('ok'); // Detect
click('ok'); // Save
let saved=JSON.parse(values.get('flipper-term-web-v1'));
assert.strictEqual(saved.saved['125 kHz RFID'].length,1);
click('back'); click('down'); click('ok'); // saved list
click('ok'); // preview
assert.ok(frame && frame.some(x=>x!==0));
click('back');click('back');click('back'); // main
// Up from first main item wraps to Settings.
click('up'); click('ok'); // Settings
click('down'); click('down'); click('ok'); // confirm reset
click('back'); assert.strictEqual(JSON.parse(values.get('flipper-term-web-v1')).saved['125 kHz RFID'].length,1);
click('ok');click('ok'); // confirm erase
saved=JSON.parse(values.get('flipper-term-web-v1')); assert.deepStrictEqual(saved.saved,{});
assert(listeners.keydown && listeners.keyup);
let prevented=false; listeners.keydown({key:'Escape',preventDefault(){prevented=true;}}); assert(prevented);
console.log('Web smoke test passed');
