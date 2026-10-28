
'use strict';
const UUID={svc:'d7b67a20-9eb5-4f13-9ea5-6bc59a0a9800',rx:'d7b67a21-9eb5-4f13-9ea5-6bc59a0a9800',tx:'d7b67a22-9eb5-4f13-9ea5-6bc59a0a9800'};
const $=id=>document.getElementById(id),enc=new TextEncoder(),dec=new TextDecoder();
let device=null,writeCh=null,readCh=null,busy=false,state=null,ticker=null,reading=false;
function log(x){const p=$('log');p.textContent=new Date().toLocaleTimeString()+' '+x+'\n'+p.textContent.slice(0,2500);}
const value=(x,unit='')=>x===undefined||x===null||Number.isNaN(Number(x))?'—':String(x)+unit;
const stateNames={0:'OFF',1:'GRZANIE',2:'CHŁODZENIE',4:'WENTYLACJA',6:'CZUWANIE',255:'BRAK DANYCH'};
function render(o){state=o;$('room').textContent=value(o.regulationTemp,'°C');$('source').textContent='Źródło: '+(o.sourceUsed||'—')+' · Webasto '+value(o.vevorRoom,'°C');$('case').textContent=o.heaterFresh?value(o.caseTemp,'°C'):'—';$('outside').textContent=value(o.tOutside,'°C');$('heaterstate').textContent=stateNames[o.heaterState]||'Stan '+o.heaterState;$('gear').textContent=o.mode===0?'H'+o.gear:(o.mode===1?'AUTO':'Tryb nieznany');$('alpine').textContent='Alpejski: '+(o.alpine===1?'TAK':o.alpine===0?'NIE':'brak danych');$('runtime').textContent=value((o.heatS||0)/3600===0?0:((o.heatS||0)/3600).toFixed(1),' h');$('fuel').textContent=value(o.fuelMl,' ml');$('starts').textContent=value(o.starts);$('frames').textContent=value(o.rxValid);$('health').textContent='ESP32 → Webasto: '+(o.heaterFresh?'POŁĄCZONY, świeże dane':'BRAK AKTUALNYCH DANYCH')+' · 21 bajtów: '+o.rxLen+' · '+(o.rxWhy||'—')+' · Pilot '+(o.armed?'UZBROJONY':'ZABLOKOWANY')+' · Termostat '+(o.thermostat?'WŁ.':'WYŁ.')+' · OTA '+(o.otaActive?'TRWA':'NIE')+' · '+(o.stopUncertain?'UWAGA: STOP NIEPOTWIERDZONY — SPRAWDŹ PIEC · ':'')+(o.result||'');$('health').className=(o.heaterFresh && !o.stopUncertain && !(o.result||'').includes('NIEPOTWIERDZONY'))?'status':'warn';$('tempsource').value=o.sourceSelected||'AUTO';if(document.activeElement!==$('target')&&Number.isFinite(o.set10))$('target').value=(o.set10/10);if(document.activeElement!==$('band')&&Number.isFinite(o.band10))$('band').value=String(o.band10/10);}
function read(){if(window.PilotNative)PilotNative.read();}
function command(c){if(!window.PilotNative)return log('Brak mostu Android');PilotNative.send(c);log('Wysłano: '+(c.startsWith('KEY:')?'KEY:********':c));setTimeout(read,700);}
function clear(){$('conn').textContent='Rozłączony';log('Rozłączono');}
window.__pilotConn=function(s){$('conn').textContent=s;log(s);};
window.__pilotStatus=function(raw){try{render(JSON.parse(raw));}catch(e){log('Błąd raportu JSON: '+e.message)}};
window.__pilotLog=function(s){log(s);};
$('connect').onclick=()=>{if(window.PilotNative)PilotNative.connect();};
$('disconnect').onclick=()=>{if(window.PilotNative)PilotNative.disconnect();clear();};
setInterval(read,4000);
$('refresh').onclick=()=>read();$('unlock').onclick=()=>{const k=$('key').value.trim();if(!/^\d{8}$/.test(k)){log('Kod dostępu: 8 cyfr widoczne po uruchomieniu ESP32 w monitorze szeregowym 115200');return}command('KEY:'+k)};
$('arm').onclick=()=>command('ARM');$('disarm').onclick=()=>command('DISARM');
document.querySelectorAll('[data-cmd]').forEach(b=>{b.onclick=()=>{if(b.dataset.confirm&&!confirm(b.dataset.confirm))return;command(b.dataset.cmd)}});
for(let i=1;i<=6;i++){const b=document.createElement('button');b.textContent='H'+i;b.onclick=()=>command('H'+i);$('gears').appendChild(b)}
$('saveTarget').onclick=()=>{const v=Number($('target').value);if(!(v>=5&&v<=35&&v*2===Math.round(v*2)))return log('Cel: 5–35°C co 0,5°C');command('TEMP:'+Math.round(v*10))};
$('saveBand').onclick=()=>command('BAND:'+Math.round(Number($('band').value)*10));
$('saveSource').onclick=()=>command('SOURCE:'+$('tempsource').value);
