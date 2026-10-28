package pl.vevor.esp32pilot;

import android.Manifest;
import android.app.Activity;
import android.os.Bundle;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.content.Context;
import android.content.pm.PackageManager;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.BluetoothLeScanner;
import android.webkit.JavascriptInterface;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.graphics.Color;
import org.json.JSONObject;
import java.nio.charset.StandardCharsets;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;

public final class MainActivity extends Activity {
  private static final UUID SVC=UUID.fromString("d7b67a20-9eb5-4f13-9ea5-6bc59a0a9800");
  private static final UUID RX =UUID.fromString("d7b67a21-9eb5-4f13-9ea5-6bc59a0a9800");
  private static final UUID TX =UUID.fromString("d7b67a22-9eb5-4f13-9ea5-6bc59a0a9800");
  private final Handler handler = new Handler(Looper.getMainLooper());
  private WebView web;
  private BluetoothLeScanner scanner;
  private BluetoothGatt gatt;
  private BluetoothGattCharacteristic writeChar, readChar;
  private final AtomicBoolean busy = new AtomicBoolean(false);
  private boolean scanning=false;
  private BluetoothAdapter adapter;
  private final ScanCallback scan = new ScanCallback() {
    @Override public void onScanResult(int callbackType, ScanResult result) {
      BluetoothDevice d=result.getDevice();String name=null;
      try { name=result.getScanRecord()==null?null:result.getScanRecord().getDeviceName();if(name==null)name=d.getName(); }catch(SecurityException ignore){}
      if(name!=null&&name.startsWith("VEVOR-ESP32")){
        stopScan(); say("Łączenie: "+name); try { gatt=d.connectGatt(MainActivity.this,false,callbacks,BluetoothDevice.TRANSPORT_LE); }
        catch(SecurityException e){say("Brak uprawnienia Bluetooth");}
      }
    }
    @Override public void onScanFailed(int errorCode) {scanning=false;say("Błąd wyszukiwania BLE: "+errorCode);}
  };
  private final BluetoothGattCallback callbacks=new BluetoothGattCallback(){
    @Override public void onConnectionStateChange(BluetoothGatt g,int status,int state){
      if(state==BluetoothProfile.STATE_CONNECTED&&status==BluetoothGatt.GATT_SUCCESS){
        say("ESP32 połączony; uzgadniam MTU");
        try { if(!g.requestMtu(185))g.discoverServices(); }catch(SecurityException e){say("Brak uprawnienia Bluetooth");}
      }else{
        writeChar=null;readChar=null;busy.set(false);say("ESP32 rozłączony ("+status+")");
        try {g.close();}catch(Exception ignore){}
        if(gatt==g)gatt=null;
      }
    }
    @Override public void onMtuChanged(BluetoothGatt g,int mtu,int status){
      say("MTU "+mtu);try{g.discoverServices();}catch(SecurityException e){say("Brak uprawnienia Bluetooth");}
    }
    @Override public void onServicesDiscovered(BluetoothGatt g,int status){
      if(status!=BluetoothGatt.GATT_SUCCESS){say("Nie znaleziono usługi");return;}
      BluetoothGattService s=g.getService(SVC);if(s==null){say("Brak usługi pilota na ESP32");return;}
      writeChar=s.getCharacteristic(RX);readChar=s.getCharacteristic(TX);
      if(writeChar==null||readChar==null){say("Brak charakterystyk pilota");return;}
      say("ESP32 gotowy");handler.postDelayed(()->readStatus(),500);
    }
    @Override public void onCharacteristicRead(BluetoothGatt g,BluetoothGattCharacteristic c,byte[] value,int status){
      receiveRead(value,status);
    }
    @SuppressWarnings("deprecation")
    @Override public void onCharacteristicRead(BluetoothGatt g,BluetoothGattCharacteristic c,int status){
      if(Build.VERSION.SDK_INT<33)receiveRead(c.getValue(),status);
    }
    @Override public void onCharacteristicWrite(BluetoothGatt g,BluetoothGattCharacteristic c,int status){
      busy.set(false);if(status!=BluetoothGatt.GATT_SUCCESS)log("Błąd zapisu BLE: "+status);
      else handler.postDelayed(()->readStatus(),600);
    }
  };
  private void receiveRead(byte[] data,int status){
    busy.set(false);
    if(status!=BluetoothGatt.GATT_SUCCESS||data==null){log("Błąd odczytu BLE: "+status);return;}
    String payload=new String(data,StandardCharsets.UTF_8);
    js("window.__pilotStatus("+JSONObject.quote(payload)+")");
  }
  private void say(String s){js("window.__pilotConn("+JSONObject.quote(s)+")");}
  private void log(String s){js("window.__pilotLog("+JSONObject.quote(s)+")");}
  private void js(String s){handler.post(()->{if(web!=null)web.evaluateJavascript(s,null);});}
  @Override public void onCreate(Bundle b){super.onCreate(b);
    web=new WebView(this);web.setBackgroundColor(Color.rgb(8,21,34));web.getSettings().setJavaScriptEnabled(true);web.setWebChromeClient(new android.webkit.WebChromeClient());
    web.getSettings().setDomStorageEnabled(true);web.addJavascriptInterface(new Bridge(),"PilotNative");setContentView(web);
    web.loadUrl("file:///android_asset/pilot.html");
    BluetoothManager mgr=(BluetoothManager)getSystemService(Context.BLUETOOTH_SERVICE);adapter=mgr==null?null:mgr.getAdapter();
    if(Build.VERSION.SDK_INT>=31 && (checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN)!=PackageManager.PERMISSION_GRANTED ||checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT)!=PackageManager.PERMISSION_GRANTED))
      requestPermissions(new String[]{Manifest.permission.BLUETOOTH_SCAN,Manifest.permission.BLUETOOTH_CONNECT},111);
    else if(Build.VERSION.SDK_INT<31&&checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION)!=PackageManager.PERMISSION_GRANTED)
      requestPermissions(new String[]{Manifest.permission.ACCESS_FINE_LOCATION},112);
  }
  public final class Bridge {
    @JavascriptInterface public void connect(){handler.post(()->startScan());}
    @JavascriptInterface public void disconnect(){handler.post(()->closeBle());}
    @JavascriptInterface public void read(){handler.post(()->readStatus());}
    @JavascriptInterface public void send(String text){handler.post(()->sendCommand(text));}
  }
  private boolean allowed(){if(Build.VERSION.SDK_INT>=31)return checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN)==PackageManager.PERMISSION_GRANTED&&checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT)==PackageManager.PERMISSION_GRANTED;
    return checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION)==PackageManager.PERMISSION_GRANTED;}
  private void startScan(){
    if(adapter==null){say("Brak Bluetooth w telefonie");return;}
    if(!allowed()){
      say("Nadaj uprawnienia Bluetooth i połącz ponownie");
      if(Build.VERSION.SDK_INT>=31)requestPermissions(new String[]{Manifest.permission.BLUETOOTH_SCAN,Manifest.permission.BLUETOOTH_CONNECT},111);
      else requestPermissions(new String[]{Manifest.permission.ACCESS_FINE_LOCATION},112);
      return;
    }
    if(!adapter.isEnabled()){say("Włącz Bluetooth w telefonie");return;}
    closeBle();scanner=adapter.getBluetoothLeScanner();if(scanner==null){say("Skaner BLE niedostępny");return;}
    say("Szukam VEVOR-ESP32…");scanning=true;try{scanner.startScan(scan);}catch(SecurityException e){say("Błąd uprawnienia skanowania");}
    handler.postDelayed(()->{if(scanning){stopScan();say("Nie znaleziono ESP32 — spróbuj ponownie");}},12000);
  }
  private void stopScan(){if(!scanning)return;scanning=false;try{if(scanner!=null)scanner.stopScan(scan);}catch(SecurityException ignore){}}
  private void closeBle(){stopScan();writeChar=null;readChar=null;busy.set(false);if(gatt!=null){try{gatt.disconnect();gatt.close();}catch(SecurityException ignore){}gatt=null;}say("Rozłączony");}
  private void sendCommand(String cmd){if(cmd==null||cmd.length()>55||!cmd.matches("[A-Za-z0-9:+.-]+")){log("Nieprawidłowe polecenie");return;}
    if(gatt==null||writeChar==null){log("Brak połączenia ESP32");return;}
    if(!busy.compareAndSet(false,true)){log("Bluetooth zajęty. Spróbuj ponownie.");return;}
    try{
      byte[] bytes=cmd.getBytes(StandardCharsets.UTF_8);int ok;
      if(Build.VERSION.SDK_INT>=33)ok=gatt.writeCharacteristic(writeChar,bytes,BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
      else{writeChar.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);writeChar.setValue(bytes);ok=gatt.writeCharacteristic(writeChar)?0:-1;}
      if(ok!=0){busy.set(false);log("Nie udało się wysłać: "+ok);}
    }catch(SecurityException e){busy.set(false);log("Brak uprawnień Bluetooth");}
  }
  private void readStatus(){if(gatt==null||readChar==null)return;
    if(!busy.compareAndSet(false,true))return;
    try{if(!gatt.readCharacteristic(readChar)){busy.set(false);log("Błąd rozpoczęcia odczytu")}}catch(SecurityException e){busy.set(false);log("Brak uprawnień odczytu");}
  }
  @Override protected void onDestroy(){closeBle();if(web!=null)web.destroy();super.onDestroy();}
}
