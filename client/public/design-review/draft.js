import { createDraft, draftSeat, pickDraftCard } from './draft-state.js';

const people = [
  { letter:'A', name:'Bạn', initials:'BC', tint:'#6e9cde' },
  { letter:'B', name:'Minh Anh', initials:'MA', tint:'#e78e7b' },
  { letter:'C', name:'Thảo Nguyên', initials:'TN', tint:'#5bb69c' },
  { letter:'D', name:'Hoàng Khang', initials:'HK', tint:'#61afc7' },
];
const colors = { red:'đỏ', yellow:'vàng', green:'xanh lá', blue:'xanh dương', wild:'đổi màu' };
const values = { draw2:'+2', skip:'cấm lượt', reverse:'đảo chiều', discard_all:'bỏ hết màu', wild:'tự do', wild4:'+4' };
const $ = selector => document.querySelector(selector);
const cardImage = card => `../images/cards/${card}.png`;
const cardLabel = card => { const [color, ...value] = card.split('_'); const name=value.join('_'); return `${colors[color]} ${values[name] ?? name}`; };
let state;
let deadline;
let botDeadline;
let timer;
let introTimer;
let handCount=0;
let pendingPick=null;
let flightTimer;
let flyingCard;
const dialog=$('#draft-dialog');
const availableOffers=()=>state.offers.flatMap((card,i)=>card?[i]:[]);

function start() {
  clearInterval(timer);
  clearTimeout(introTimer);
  clearTimeout(flightTimer);
  flyingCard?.remove();
  pendingPick=null;
  delete dialog.dataset.flying;
  document.body.dataset.phase='table';
  if (dialog.open) dialog.close();
  $('#resume').hidden=true;
  $('#table-phase').textContent='Đang chuẩn bị chọn bài…';
  const cards=['red','yellow','green','blue'].flatMap(color=>['1','3','5','7','9','reverse','draw2','skip','discard_all'].map(value=>`${color}_${value}`));
  cards.push('wild_wild','wild_wild4');
  for (let i=cards.length-1;i>0;i--) { const j=Math.floor(Math.random()*(i+1)); [cards[i],cards[j]]=[cards[j],cards[i]]; }
  state=createDraft(cards);
  handCount=0;
  $('#last-pick').textContent='Bốn lá khởi đầu đã được chia.';
  render();
  introTimer=setTimeout(openPick,850);
}
function openPick() {
  clearTimeout(introTimer);
  clearInterval(timer);
  document.body.dataset.phase='opening';
  $('#resume').hidden=true;
  $('#table-phase').textContent='Chọn bài đầu ván · 4 vòng';
  render();
  if (!dialog.open) dialog.showModal();
  introTimer=setTimeout(()=>{
    document.body.dataset.phase='picking';
    beginTurn();
    timer=setInterval(tick,100);
    $('.offer:enabled')?.focus();
  },320);
}
function beginTurn() {
  deadline=Date.now()+10000;
  botDeadline=Date.now()+(availableOffers().length===1?600:1300);
  render();
  tick();
}
function pick(index, automatic=false) {
  const seat=draftSeat(state);
  if (seat===null || pendingPick || document.body.dataset.phase!=='picking') return;
  const forced=availableOffers().length===1;
  if (forced&&!automatic) return;
  const card=state.offers[index];
  const next=pickDraftCard(state,index);
  if (next===state) return;
  pendingPick={next,seat,card,automatic,forced};
  animatePick(index,seat,card);
}
function animatePick(index,seat,card) {
  const source=$(`[data-offer="${index}"]`).getBoundingClientRect();
  let target;
  if (seat===0) {
    const hand=$('#my-hand').getBoundingClientRect();
    const image=$('.hand-card');
    const count=state.hands[0].length+1;
    const span=count*image.offsetWidth-(count-1)*18;
    target={left:hand.left+hand.width/2+span/2-image.offsetWidth,top:hand.bottom-image.offsetHeight,width:image.offsetWidth,height:image.offsetHeight};
  } else target=$(`[data-seat="${seat}"] .picked-slot:nth-child(${state.hands[seat].length-3})`).getBoundingClientRect();
  const reduce=matchMedia('(prefers-reduced-motion: reduce)').matches;
  const duration=reduce?140:420;
  flyingCard=document.createElement('img');
  flyingCard.src=cardImage(card);
  flyingCard.alt='';
  flyingCard.className='flight-card';
  flyingCard.setAttribute('aria-hidden','true');
  flyingCard.setAttribute('popover','manual');
  flyingCard.style.cssText=`left:${source.left}px;top:${source.top}px;width:${source.width}px;height:${source.height}px;--flight-x:${target.left-source.left}px;--flight-y:${target.top-source.top}px;--flight-scale:${Math.min(target.width/source.width,target.height/source.height)};--flight-duration:${duration}ms`;
  document.body.append(flyingCard);
  flyingCard.showPopover();
  dialog.dataset.flying='true';
  $(`[data-offer="${index}"]`).classList.add('departing');
  document.querySelectorAll('.offer').forEach(el=>el.disabled=true);
  flightTimer=setTimeout(finishPick,duration);
}
function finishPick() {
  if (!pendingPick) return;
  clearTimeout(flightTimer);
  const {next,seat,card,automatic,forced}=pendingPick;
  const newRound=next.turn<16&&next.turn%4===0;
  pendingPick=null;
  flyingCard?.remove();
  delete dialog.dataset.flying;
  state=next;
  $('#last-pick').textContent=`${people[seat].name} ${forced?'tự nhận lá còn lại:':automatic&&seat===0?'hết giờ, tự chọn':'đã chọn'} lá ${cardLabel(card)}.`;
  if (draftSeat(state)===null) {
    clearInterval(timer);
    document.body.dataset.phase='complete';
    $('#table-phase').textContent='Hoàn tất chọn bài · Mỗi người 8 lá';
    render();
    dialog.close();
    $('#try-again').focus();
  } else if (document.body.dataset.phase==='picking') {
    beginTurn();
    if (newRound) document.querySelectorAll('.offer').forEach(el=>el.classList.add('new-round'));
    if (draftSeat(state)===0) $('.offer:enabled')?.focus();
  } else render();
}
function tick() {
  const seat=draftSeat(state);
  if (seat===null || pendingPick) return;
  const available=availableOffers();
  const forced=available.length===1;
  const remaining=Math.max(0,Math.ceil((deadline-Date.now())/1000));
  $('#clock').textContent=forced?'AUTO':`${remaining}s`;
  $('#clock').classList.toggle('urgent',remaining<=3);
  if (Date.now()>=deadline || ((seat!==0||forced) && Date.now()>=botDeadline)) pick(available[Math.floor(Math.random()*available.length)],true);
}
function render() {
  const seat=draftSeat(state);
  const done=seat===null;
  const round=Math.min(3,Math.floor(state.turn/4));
  const step=state.turn%4;
  const remaining=availableOffers().length;
  const forced=remaining===1;
  dialog.dataset.turn=done?'none':['bottom','left','top','right'][seat];
  $('#players').innerHTML=people.map((p,i)=>`<div class="draft-player seat-${['bottom','left','top','right'][i]} ${seat===i?'active':''}" data-seat="${i}" ${seat===i?'aria-current="true"':''}><div class="player-identity"><svg class="turn-marker" viewBox="0 0 40 40" aria-hidden="true"><path d="M12 3h16v15h10L20 37 2 18h10Z" fill="#ffd85e" stroke="#fff1b9" stroke-width="2" stroke-linejoin="round"/></svg><span class="draft-avatar" style="--tint:${p.tint}">${p.initials}<span class="card-count" aria-label="${state.hands[i].length} lá">${state.hands[i].length}</span></span><b>${p.name}</b><small>${done?'Sẵn sàng':seat===i?'Đang chọn bài':'Chờ tới lượt'}</small></div><div class="picked-tray" aria-label="Bài đã pick của ${p.name}">${Array.from({length:4},(_,slot)=>{const card=state.hands[i][4+slot];return `<span class="picked-slot">${card?`<img src="${cardImage(card)}" alt="${cardLabel(card)}">`:'·'}</span>`;}).join('')}</div></div>`).join('');
  $('#round-label').textContent=done?'4 / 4':`Vòng ${round+1} / 4`;
  $('#pick-title').textContent=done?'Chọn bài hoàn tất':forced?`${people[seat].name} nhận lá cuối cùng`:seat===0?'Đến lượt bạn chọn bài':`${people[seat].name} đang chọn…`;
  $('#pick-hint').textContent=done?'Mỗi người đều đã có một lần được chọn đầu tiên.':forced?'Người cuối tự nhận lá còn lại. Vòng sau mới mở 4 lá mới.':`Còn ${remaining} lá để chọn. Mỗi vòng chỉ có 4 lá, không bù giữa vòng.`;
  $('#turn-order').innerHTML=Array.from({length:4},(_,i)=>{const p=people[(round+i)%4];return `${i?'<span class="order-arrow">→</span>':''}<span class="order-player ${!done&&i===step?'active':''} ${done||i<step?'picked':''}"><b>${p.letter}</b>${p.name}</span>`;}).join('');
  $('#offers').hidden=done;
  $('#offers').setAttribute('aria-label',`${remaining} lá còn lại trong vòng`);
  $('#clock').hidden=done;
  $('#complete').hidden=!done;
  $('#offers').innerHTML=state.offers.map((card,i)=>card?`<button class="offer" data-offer="${i}" aria-label="Chọn lá ${cardLabel(card)}" ${seat===0&&!forced&&document.body.dataset.phase==='picking'?'':'disabled'}><img src="${cardImage(card)}" alt=""></button>`:`<span class="offer-empty" aria-label="Lá đã được chọn"></span>`).join('');
  document.querySelectorAll('[data-offer]').forEach(el=>el.addEventListener('click',()=>{if(draftSeat(state)===0)pick(Number(el.dataset.offer));}));
  $('#draft-progress').textContent=`${state.turn} / 16 lượt chọn`;
  $('#hand-count').textContent=`${state.hands[0].length} / 8`;
  $('#my-hand').innerHTML=state.hands[0].map((card,i)=>`<img class="hand-card ${i>=4?'picked':''} ${i>=handCount?'just-dealt':''}" style="--angle:${(i-(state.hands[0].length-1)/2)*2}deg" src="${cardImage(card)}" alt="${cardLabel(card)}" title="${cardLabel(card)}">`).join('');
  handCount=state.hands[0].length;
}
$('#restart').addEventListener('click',start);
$('#try-again').addEventListener('click',start);
$('#resume').addEventListener('click',openPick);
dialog.addEventListener('close',()=>{
  if (!['picking','opening'].includes(document.body.dataset.phase)) return;
  clearInterval(timer);
  clearTimeout(introTimer);
  document.body.dataset.phase='paused';
  finishPick();
  if (draftSeat(state)===null) return;
  $('#table-phase').textContent='Đã tạm dừng chọn bài';
  $('#resume').hidden=false;
  $('#resume').focus();
});
window.addEventListener('pagehide',()=>{clearInterval(timer);clearTimeout(introTimer);clearTimeout(flightTimer);flyingCard?.remove();});
start();
