/* Execute the shipped inline script, with only browser/network boundaries faked. */
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const html=fs.readFileSync(process.argv[2],'utf8');
const script=html.match(/<script>([\s\S]*?)<\/script>/)[1];
class Element {
 constructor(tag='div'){this.tagName=tag.toUpperCase();this.children=[];this.attributes={};this.value='';this.disabled=false;this.hidden=false;this.open=false;this.style={setProperty(){}};this.className='';this._text='';this.classList={toggle:(n,on)=>{const s=new Set(this.className.split(/\s+/).filter(Boolean));on?s.add(n):s.delete(n);this.className=[...s].join(' ');}};}
 set textContent(v){this._text=String(v);this.children=[];}get textContent(){return this._text+this.children.map(x=>x.textContent).join('');}
 set innerHTML(v){throw new Error('Untrusted markup assignment');}
 append(...v){this.children.push(...v);}replaceChildren(...v){this._text='';this.children=v;}
 setAttribute(k,v){this.attributes[k]=String(v);}focus(){this.focused=true;}showModal(){this.open=true;}close(){this.open=false;if(this.onclose)this.onclose();}
 addEventListener(k,v){this['on'+k]=v;}scrollIntoView(){}
}
const evil='Home <img src=x onerror=alert(1)> & 家';
const choices=[{id:'486f6d65',ssid:evil,secure:true},{id:'4775657374',ssid:'Guest',secure:false}];
const tick=async()=>{for(let i=0;i<20;i++)await Promise.resolve();};
async function page({pending=0}={}){
 const elements={};for(const m of html.matchAll(/id="([^"]+)"/g))elements[m[1]]=new Element();
 let scans=choices,scanMode='ok',post=202,statuses=[{state:'connecting',error:'none'}],posts=[],calls=[],timers=[],now=0;
 const fetch=async(url,options={})=>{calls.push(url);if(url==='/networks'||url==='/networks?refresh=1'){if(scanMode==='offline')throw Error('offline');const waiting=pending-->0||scanMode==='pending';return {ok:scanMode!=='http',status:waiting?202:200,json:async()=>waiting?{scanning:true}:{networks:scans}};}if(url==='/wifi'){posts.push(JSON.parse(options.body));if(post==='offline')throw Error('offline');return {ok:post===202,status:post,json:async()=>({})};}assert.equal(url,'/wifi/status');const s=statuses.length>1?statuses.shift():statuses[0];if(s==='offline')throw Error('offline');return {ok:true,json:async()=>s};};
 const context={document:{getElementById:id=>elements[id],createElement:t=>new Element(t),documentElement:new Element(),activeElement:null},window:{addEventListener(){}},fetch,TextEncoder,AbortController,Date:{now:()=>now},setTimeout:(f,ms)=>{const t={f,ms};timers.push(t);return t;},clearTimeout:t=>{timers=timers.filter(x=>x!==t);},requestAnimationFrame:f=>f()};
 vm.createContext(context);vm.runInContext(script,context);await tick();
 return {e:elements,c:context,posts,calls,setScans:v=>{scans=v;},setScan:v=>{scanMode=v;},setPost:v=>{post=v;},setStatus:v=>{statuses=v;},choose:i=>{const row=elements.choices.children[i];if(row.onclick)return row.onclick();const input=row.children[0];input.checked=true;return input.onchange();},submit:()=>elements.f.onsubmit({preventDefault(){}}),next:async()=>{const i=timers.findIndex(t=>t.ms<=1500);assert(i>=0,'Expected another status poll');const t=timers.splice(i,1)[0];now+=t.ms;t.f();await tick();}};
}
(async()=>{
 const waiting=await page({pending:2});
 assert(!waiting.e['scan-progress'].hidden&&waiting.e.scan.disabled);
 assert(waiting.e['scan-label'].textContent.includes('正在查找附近的 2.4G 网络'));assert.equal(waiting.e.choices.children.length,0);
 await waiting.next();assert(!waiting.e['scan-progress'].hidden);await waiting.next();
 assert(waiting.e['scan-progress'].hidden&&!waiting.e.scan.disabled&&waiting.e.choices.children.length===2);
 assert.deepEqual(waiting.calls,['/networks','/networks','/networks'],'First visit and polls can reuse the shared device scan');
 waiting.setScan('pending');const refresh=waiting.e.scan.onclick();await tick();
 assert.equal(waiting.calls.at(-1),'/networks?refresh=1');assert(!waiting.e['scan-progress'].hidden);
 await waiting.next();assert.equal(waiting.calls.at(-1),'/networks');waiting.setScan('ok');await waiting.next();await refresh;
 assert(waiting.e['scan-progress'].hidden&&!waiting.e.scan.disabled);
 const stalled=await page({pending:100});for(let i=0;i<35;i++)await stalled.next();
 assert(stalled.e['scan-progress'].hidden&&!stalled.e.scan.disabled&&stalled.e.r.textContent.includes('扫描失败'));
 const interrupted=await page({pending:5});interrupted.e.manual.onclick();
 interrupted.e['manual-name'].value='Hidden';interrupted.e['manual-pass'].value='password';interrupted.setStatus([{state:'connected',error:'none'}]);
 await interrupted.submit();const result=interrupted.e.r.textContent;await interrupted.next();
 assert(result.includes('已连接并保存')&&interrupted.e.r.textContent===result,'Stale scan cannot overwrite a manual connection result');
 assert(interrupted.e['scan-progress'].hidden&&interrupted.e.choices.children.length===0);
 const manual=await page(),m=manual.e;
 manual.setScans([]);await m.scan.onclick();m.manual.onclick();
 assert.equal(m['manual-form'].hidden,false);assert.equal(m['near-panel'].hidden,true);
 m['manual-name'].value='中'.repeat(11);m['manual-pass'].value='12345678';
 await manual.submit();assert(!manual.posts.length&&m.r.textContent.includes('32'));
 m['manual-name'].value=' Cam 隐藏 ';manual.setStatus([{state:'failed',error:'not_found'}]);
 await manual.submit();assert.deepEqual(manual.posts[0],{ssid:' Cam 隐藏 ',password:'12345678'});
 assert(m.r.textContent.includes('大小写')&&m['manual-name'].value===' Cam 隐藏 ');
 m['manual-open'].checked=true;m['manual-open'].onchange();assert(m['manual-pw'].hidden);
 manual.setStatus([{state:'connected',error:'none'}]);await manual.submit();
 assert.deepEqual(manual.posts[1],{ssid:' Cam 隐藏 ',password:''});assert(m.r.textContent.includes('已连接并保存'));
 const p=await page(),e=p.e;
 assert.equal(e.choices.children.length,2);assert(e.choices.children[0].textContent.includes(evil));
 p.choose(0);assert(e.prompt&&e.prompt.open,'Selection must immediately open the password dialog');assert(e.p.focused,'Password gets focus from user selection');assert.equal(e.ssid.textContent,evil);assert(!e.pw.hidden);
 e.p.value='七七七';p.setStatus([{state:'connecting',error:'none'},{state:'failed',error:'auth'}]);await p.submit();
 assert.deepEqual(p.posts[0],{ssid_hex:'486f6d65',password:'七七七'});assert(e.scan.disabled);assert(e.connect.disabled);assert(e.prompt.open);assert.equal(e.p.value,'七七七');assert(!e.r.textContent.includes('已连接并保存'),'202 is acceptance only');
 await p.next();assert(e.r.textContent.includes('密码'));assert(!e.connect.disabled&&!e.scan.disabled);assert.equal(e.p.value,'七七七');
 for(const invalid of ['','1234567','x'.repeat(64),'七'.repeat(22)]){e.p.value=invalid;const count=p.posts.length;await p.submit();assert.equal(p.posts.length,count,'Validate UTF-8 byte count before POST');}
 e.p.value='七'.repeat(21);p.setStatus([{state:'connected',error:'none'}]);await p.submit();assert.equal(p.posts.length,2);assert(e.r.textContent.includes('已连接并保存'));assert(e.r.textContent.includes('设备屏幕'));assert.equal(e.p.value,'七'.repeat(21));
 const open=await page();open.choose(1);assert(open.e.prompt.open&&open.e.pw.hidden);open.e.p.value='must not send';open.setStatus([{state:'failed',error:'not_found'}]);await open.submit();assert.equal(open.posts[0].password,'');
 const uncertain=await page();uncertain.choose(0);uncertain.e.p.value='unchanged';uncertain.setStatus(['offline']);await uncertain.submit();await uncertain.next();await uncertain.next();assert(uncertain.e.r.textContent.includes('尚未确认'));assert(!uncertain.e.check.hidden);assert(uncertain.e.scan.disabled);assert.equal(uncertain.e.p.value,'unchanged');uncertain.setStatus([{state:'failed',error:'save_failed'}]);await uncertain.e.check.onclick();assert(uncertain.e.r.textContent.includes('保存'));assert(!uncertain.e.connect.disabled);
 const lostPost=await page();lostPost.choose(0);lostPost.e.p.value='password';lostPost.setPost('offline');lostPost.setStatus([{state:'connected',error:'none'}]);await lostPost.submit();assert(lostPost.e.r.textContent.includes('已连接并保存'),'Lost POST reply must reconcile actual status');
 const busy=await page();busy.choose(0);busy.e.p.value='password';busy.setPost(409);busy.setStatus([{state:'connecting',error:'none'}]);await busy.submit();assert(busy.e.scan.disabled&&busy.e.connect.disabled);busy.e.cancel.onclick();assert(!busy.e.prompt.open);assert(!busy.e.resume.hidden);busy.e.resume.onclick();assert(busy.e.prompt.open);assert.equal(busy.e.p.value,'password');
 const invalid=await page();invalid.choose(0);invalid.e.p.value='password';invalid.setPost(400);await invalid.submit();assert(!invalid.e.connect.disabled);assert(invalid.e.r.textContent.includes('格式'));assert.equal(invalid.e.p.value,'password');
 const scan=await page();scan.choose(0);scan.e.p.value='retain me';scan.e.cancel.onclick();scan.setScans([choices[1],choices[0]]);await scan.c.refresh();assert.equal(scan.e.p.value,'retain me');assert(scan.e.choices.children[1].className.includes('selected'));scan.setScan('offline');await scan.c.refresh();assert.equal(scan.e.choices.children.length,2);assert(!scan.e.scan.disabled);scan.setScan('ok');scan.setScans([]);await scan.c.refresh();assert.equal(scan.e.choices.children.length,0);assert(scan.e.resume.hidden&&scan.e.p.value==='');
 for(const error of ['timeout','unknown']){const fail=await page();fail.choose(0);fail.e.p.value='password';fail.setStatus([{state:'failed',error}]);await fail.submit();assert(!fail.e.connect.disabled);assert(!fail.e.r.textContent.includes('已连接并保存'));}
 console.log('portal behavior PASS: async scan progress/cache/timeout/manual interruption, modal/focus, UTF-8, open/secure, accepted/status/failure/retry, lost handoff, preserved input');
})().catch(e=>{console.error(e);process.exit(1);});
