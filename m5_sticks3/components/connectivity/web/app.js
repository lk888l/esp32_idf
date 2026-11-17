'use strict';
const $=id=>document.getElementById(id);
let token=sessionStorage.getItem('stick.session')||'',seq=Number(sessionStorage.getItem('stick.seq')||0);
let ws=null,wanted=false,reconnect=null,queue=Promise.resolve(),samples=[],lastAt=0,lastSeq=0,boot=null,received=0,rateAt=performance.now(),stale=false;
const logs=[];let polling=false;
function log(text){logs.unshift(`${new Date().toLocaleTimeString()}  ${text}`);logs.length=Math.min(logs.length,8);$('log').textContent=logs.slice(0,8).join('\n');}
function notice(text){$('notice').textContent=text;}
function state(text){$('connection').textContent=text;}
function authUI(){ $('pair').classList.toggle('hidden',!!token);$('paired').classList.toggle('hidden',!token);for(const id of ['start','stop','configure'])$(id).disabled=!token; }
function clearAuth(){token='';wanted=false;sessionStorage.removeItem('stick.session');sessionStorage.removeItem('stick.seq');seq=0;if(ws)ws.close();authUI();state('需要授权');}
async function request(path,body,headers={}){
 const response=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json',...headers},body:JSON.stringify(body),signal:AbortSignal.timeout(6000)});
 const data=await response.json();if(response.status===401){clearAuth();throw Error('授权已失效，请在设备上重新获取授权码');}
 if(!response.ok||data.ok===false)throw Error(data.error||`HTTP ${response.status}`);return data;
}
function command(op,fields={}){
 const run=async()=>{if(!token)throw Error('请先授权');const n=++seq;sessionStorage.setItem('stick.seq',String(seq));
 const payload={v:1,id:n,op,...fields},headers={Authorization:`Bearer ${token}`,'X-Command-Seq':String(n)};
 // A transport retry uses exactly the same sequence and bytes; never retry a
 // mutation with a new identity after losing its response.
 try{return await request('/api/v1/control',payload,headers);}catch(e){if(e instanceof TypeError||e.name==='TimeoutError')return request('/api/v1/control',payload,headers);throw e;}};
 const next=queue.then(run,run);queue=next.catch(()=>{});return next;
}
function connect(){
 clearTimeout(reconnect);if(!token||!wanted)return;
 if(ws&&(ws.readyState===0||ws.readyState===1))return;
 state('连接数据通道…');const socket=new WebSocket(`ws://${location.host}/api/v1/stream`);ws=socket;
 socket.onopen=()=>socket.send(JSON.stringify({op:'subscribe',session:token}));
 socket.onmessage=e=>{let data;try{data=JSON.parse(e.data);}catch{return;}
  if(data.event==='subscribed'){lastAt=performance.now();state('实时连接');notice('采集已开启');log('实时通道已连接');return;}
  if(data.event==='stream.status'){if(wanted&&!data.valid){state('等待有效传感器数据');notice('传感器正在启动或暂时不可用');}return;}
  if(data.event!=='motion')return;
  if(boot!==data.boot){boot=data.boot;lastSeq=0;samples=[];}
  for(const row of data.samples){if(lastSeq&&row[0]!==lastSeq+1)log(`检测到数据缺口：${row[0]-lastSeq-1} 个样本`);lastSeq=row[0];samples.push(row);received++;}
  if(samples.length>600)samples.splice(0,samples.length-600);lastAt=performance.now();stale=false;
  const latest=samples.at(-1);if(latest)for(let i=0;i<3;i++)$(['roll','pitch','yaw'][i]).textContent=latest[i+2].toFixed(1);
  $('loss').textContent=`设备丢样 ${data.lost}`;state(data.valid?'实时连接':'传感器数据无效');if(data.valid)notice('采集已开启');
 };
 socket.onclose=()=>{if(ws!==socket)return;ws=null;if(wanted&&token){state('连接中断，正在恢复');reconnect=setTimeout(connect,1500);}else state(token?'已暂停':'等待授权');};
 socket.onerror=()=>socket.close();
}
$('pair').onsubmit=async e=>{e.preventDefault();try{const d=await request('/api/v1/session',{code:Number($('code').value)});token=d.session;seq=0;sessionStorage.setItem('stick.session',token);sessionStorage.setItem('stick.seq','0');$('code').value='';authUI();log('已获得临时授权');wanted=true;connect();}catch(e){notice(e.message);}};
$('revoke').onclick=async()=>{try{await request('/api/v1/session',{op:'revoke'},{Authorization:`Bearer ${token}`});clearAuth();notice('授权已撤销');}catch(e){notice(e.message);}};
$('start').onclick=()=>{wanted=true;connect();};
$('stop').onclick=()=>{wanted=false;clearTimeout(reconnect);if(ws)ws.close();state('已暂停');notice('采集订阅已释放');};
$('wifi').onsubmit=async e=>{e.preventDefault();try{notice('正在提交连接请求…');const d=await command('wifi.connect',{ssid:$('ssid').value,password:$('password').value});$('password').value='';log(`配网命令已接收：${d.ticket}`);notice('已提交，等待设备连接网络');for(let i=0;i<12;i++){await new Promise(r=>setTimeout(r,500));const result=await command('command.result',{ticket:d.ticket});if(result.state==='failed')throw Error(result.error_name);if(result.state==='applied'){notice('设备已应用配置，正在连接。请查看设备 IP；取得地址后才会保存。');break;}}}catch(e){notice(e.message);}};
setInterval(async()=>{if(!token||polling)return;polling=true;try{const d=await command('status');$('device').textContent=JSON.stringify(d,null,2);if(!wanted)state('已授权 · 已暂停');}catch(e){notice(e.message);}finally{polling=false;}},5000);
setInterval(()=>{const now=performance.now();$('rate').textContent=`${(received*1000/(now-rateAt)).toFixed(0)} 样本/秒`;rateAt=now;received=0;if(wanted&&lastAt&&now-lastAt>1500&&!stale){stale=true;state('数据已过期');notice('暂未收到新样本，请检查设备状态');}},1000);
const canvas=$('chart'),ctx=canvas.getContext('2d');
function draw(){const w=canvas.clientWidth,h=canvas.clientHeight,dpr=devicePixelRatio||1;if(canvas.width!==Math.round(w*dpr)||canvas.height!==Math.round(h*dpr)){canvas.width=Math.round(w*dpr);canvas.height=Math.round(h*dpr);}ctx.setTransform(dpr,0,0,dpr,0,0);ctx.clearRect(0,0,w,h);ctx.font='11px system-ui';ctx.strokeStyle='#324139';ctx.fillStyle='#a4b6a8';for(const degree of [-180,-90,0,90,180]){let y=12+(180-degree)/360*(h-24);ctx.beginPath();ctx.moveTo(35,y);ctx.lineTo(w,y);ctx.stroke();ctx.fillText(String(degree),0,y+4);}const colors=['#c4f18b','#85c9f2','#efb899'];for(let axis=0;axis<3;axis++){ctx.strokeStyle=colors[axis];ctx.lineWidth=1.5;ctx.beginPath();let previous=0;const latest=samples.at(-1)?.[1]||0;for(const s of samples){const x=35+(1-(latest-s[1])/6000000)*(w-35),y=12+(180-s[axis+2])/360*(h-24);if(x<35)continue;if(!previous||s[0]!==previous+1||!s[5])ctx.moveTo(x,y);else ctx.lineTo(x,y);previous=s[0];}ctx.stroke();}}
setInterval(()=>{if(!document.hidden)draw();},40);
authUI();if(token){wanted=true;connect();}log('设备本地页面已就绪');
