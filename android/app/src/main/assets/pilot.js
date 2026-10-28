
'use strict';
const UUID={svc:'d7b67a20-9eb5-4f13-9ea5-6bc59a0a9800',rx:'d7b67a21-9eb5-4f13-9ea5-6bc59a0a9800',tx:'d7b67a22-9eb5-4f13-9ea5-6bc59a0a9800'};
const $=id=>document.getElementById(id),enc=new TextEncoder(),dec=new TextDecoder();
let device=null,writeCh=null,readCh=null,busy=false,state=null,ticker=null,reading=false;
let gearTarget=null,gearSince=0,gearFeedback="";
let gearButtons=[];
function log(x){const p=$('log');p.textContent=new Date().toLocaleTimeString()+' '+x+'\n'+p.textContent.slice(0,2500);}
const value=(x,unit='')=>x===undefined||x===null||Number.isNaN(Number(x))?'—':String(x)+unit;
const stateNames={0:'OFF',1:'GRZANIE',2:'CHŁODZENIE',4:'WENTYLACJA',6:'CZUWANIE',255:'BRAK DANYCH'};
function render(o){state=o;$('room').textContent=value(o.regulationTemp,'°C');$('source').textContent='Źródło: '+(o.sourceUsed||'—')+' · Webasto '+value(o.vevorRoom,'°C');$('case').textContent=o.heaterFresh?value(o.caseTemp,'°C'):'—';$('outside').textContent=value(o.tOutside,'°C');$('heaterstate').textContent=stateNames[o.heaterState]||'Stan '+o.heaterState;$('gear').textContent=o.mode===0?'H'+o.gear:(o.mode===1?'AUTO':'Tryb nieznany');$('alpine').textContent='Alpejski: '+(o.alpine===1?'TAK':o.alpine===0?'NIE':'brak danych');$('runtime').textContent=value((o.heatS||0)/3600===0?0:((o.heatS||0)/3600).toFixed(1),' h');$('fuel').textContent=value(o.fuelMl,' ml');$('starts').textContent=value(o.starts);$('frames').textContent=value(o.rxValid);$('health').textContent='ESP32 → Webasto: '+(o.heaterFresh?'POŁĄCZONY, świeże dane':'BRAK AKTUALNYCH DANYCH')+' · 21 bajtów: '+o.rxLen+' · '+(o.rxWhy||'—')+' · Pilot '+(o.armed?'UZBROJONY':'ZABLOKOWANY')+' · Termostat '+(o.thermostat?'WŁ.':'WYŁ.')+' · OTA '+(o.otaActive?'TRWA':'NIE')+' · '+(o.stopUncertain?'UWAGA: STOP NIEPOTWIERDZONY — SPRAWDŹ PIEC · ':'')+(o.result||'');$('health').className=(o.heaterFresh && !o.stopUncertain && !(o.result||'').includes('NIEPOTWIERDZONY'))?'status':'warn';$('tempsource').value=o.sourceSelected||'AUTO';if(document.activeElement!==$('target')&&Number.isFinite(o.set10))$('target').value=(o.set10/10);if(document.activeElement!==$('band')&&Number.isFinite(o.band10))$('band').value=String(o.band10/10);renderPower(o);}
function renderPower(o){
  const valid=!!(o && o.heaterFresh && o.heaterState===1 && o.mode===0 && o.gear>=1 && o.gear<=6);
  const current=valid?Number(o.gear):null;
  const enabled=!!(valid && o.armed && !o.thermostat);
  if(gearTarget!==null && o){
    if(valid && current===gearTarget){
      gearFeedback='H'+gearTarget+' POTWIERDZONE — poziom osiągnięty.';
      gearTarget=null;
    }else if(['H_ODRZUCONE','H_ZABLOKOWANE','H_PRZERWANE'].includes(o.result)){
      gearFeedback='Zmiana odrzucona ('+o.result+'). Bieg nie został zmieniony.';
      gearTarget=null;
    }else if(Date.now()-gearSince>75000){
      gearFeedback='Brak potwierdzenia zmiany H. Sprawdź stan pieca przed kolejną próbą.';
      gearTarget=null;
    }else if(!enabled){
      gearFeedback='Przerwano oczekiwanie — brak warunków do zmiany mocy.';
      gearTarget=null;
    }
  }
  $('gearReading').textContent=valid?'AKTUALNIE: H'+current:'BIEG: '+(o&&o.heaterState===0?'PIEC WYŁĄCZONY':'BRAK POTWIERDZONEGO POZIOMU');
  $('gearPending').textContent=gearTarget!==null?
    'ŻĄDANY: H'+gearTarget+' · aktualnie H'+current+' · czekam na potwierdzenie zmian':
    (gearFeedback || (enabled?'Wybierz H1–H6. Po zmianie sprawdź zielony poziom.': 'Zmiana H1–H6 jest dostępna tylko przy GRZANIU w trybie ręcznym, po uzbrojeniu pilota.'));
  $('gearPending').classList.toggle('warn',gearTarget!==null || gearFeedback.includes('Brak') || gearFeedback.includes('odrzucona'));
  gearButtons.forEach((b,i)=>{
    b.classList.toggle('active',valid && i+1===current);
    b.classList.toggle('waiting',gearTarget===i+1 && current!==i+1);
    b.disabled=!enabled || gearTarget!==null;
    b.setAttribute('aria-pressed',String(valid && i+1===current));
    b.title=valid && i+1===current?'Aktualny potwierdzony poziom':'Ustaw H'+(i+1);
  });
}
function read(){if(window.PilotNative)PilotNative.read();}
function command(c){if(!window.PilotNative)return log('Brak mostu Android');PilotNative.send(c);log('Wysłano: '+(c.startsWith('KEY:')?'KEY:********':c));setTimeout(read,700);}
function clear(){$('conn').textContent='Rozłączony';log('Rozłączono');}
window.__pilotConn=function(s){
  $('conn').textContent=s;
  log(s);
  // BLE can disconnect while the last ECU status still says HEATING/CONNECTED.
  // Never show that cached reading as live data after losing ESP32.
  if(s!=='ESP32 gotowy'){
    const stopAlarm=state && state.stopUncertain;
    $('health').textContent='BRAK AKTUALNEGO POŁĄCZENIA ESP32 — DANE NIEAKTUALNE'+
      (stopAlarm?' · STOP NIEPOTWIERDZONY — SPRAWDŹ PIEC':'');
    $('health').className='warn';
    $('room').textContent='—';
    $('case').textContent='—';
    $('outside').textContent='—';
    $('heaterstate').textContent='BRAK DANYCH';
    $('gear').textContent='—';
    $('alpine').textContent='Alpejski: brak aktualnego stanu';
    state=null;gearTarget=null;gearFeedback="";renderPower(null);
  }
};
window.__pilotStatus=function(raw){try{render(JSON.parse(raw));}catch(e){log('Błąd raportu JSON: '+e.message)}};
window.__pilotLog=function(s){log(s);};
$('connect').onclick=()=>{if(window.PilotNative)PilotNative.connect();};
$('disconnect').onclick=()=>{if(window.PilotNative)PilotNative.disconnect();clear();};
setInterval(read,4000);
$('refresh').onclick=()=>read();$('unlock').onclick=()=>{const k=$('key').value.trim();if(!/^\d{8}$/.test(k)){log('Kod dostępu: 8 cyfr widoczne po uruchomieniu ESP32 w monitorze szeregowym 115200');return}command('KEY:'+k)};
$('arm').onclick=()=>command('ARM');$('disarm').onclick=()=>command('DISARM');
document.querySelectorAll('[data-cmd]').forEach(b=>{b.onclick=()=>{if(b.dataset.confirm&&!confirm(b.dataset.confirm))return;command(b.dataset.cmd)}});
for(let i=1;i<=6;i++){
  const b=document.createElement('button');b.type='button';b.textContent='H'+i;b.className='gear-btn';
  b.onclick=()=>{
    if(!state||!state.heaterFresh||state.heaterState!==1||state.mode!==0||!state.armed||state.thermostat){
      log('Nie można zmieniać H: wymagane GRZANIE, tryb ręczny i uzbrojony pilot.');return;
    }
    if(gearTarget!==null){log('Zmiana H już trwa. Poczekaj na potwierdzenie.');return;}
    if(Number(state.gear)===i){gearFeedback='H'+i+' jest już aktualnym poziomem.';renderPower(state);return;}
    gearTarget=i;gearSince=Date.now();gearFeedback='';
    renderPower(state);
    command('H'+i);
  };
  gearButtons.push(b);$('gears').appendChild(b);
}
renderPower(null);
$('saveTarget').onclick=()=>{const v=Number($('target').value);if(!(v>=5&&v<=35&&v*2===Math.round(v*2)))return log('Cel: 5–35°C co 0,5°C');command('TEMP:'+Math.round(v*10))};
$('saveBand').onclick=()=>command('BAND:'+Math.round(Number($('band').value)*10));
$('saveSource').onclick=()=>command('SOURCE:'+$('tempsource').value);
