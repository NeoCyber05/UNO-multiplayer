const ranks = [
  { key:'bronze', name:'Đồng', min:0, max:1199, shape:'Huy chương · Một ngôi sao', color:'#9c6137' },
  { key:'silver', name:'Bạc', min:1200, max:1399, shape:'Khiên nhọn · Hai vạch', color:'#647f94' },
  { key:'gold', name:'Vàng', min:1400, max:1599, shape:'Khiên ngũ giác · Ngôi sao', color:'#a77820' },
  { key:'platinum', name:'Bạch kim', min:1600, max:1799, shape:'Khiên có cánh · Ngọc lục bảo', color:'#228d7e' },
  { key:'diamond', name:'Kim cương', min:1800, max:1999, shape:'Bát giác · Kim cương đa diện', color:'#3e83bc' },
  { key:'master', name:'Cao thủ', min:2000, max:Infinity, shape:'Nguyệt quế · Vương miện', color:'#c6644c' },
];
const players = [
  ['Khánh Linh','KL',2140],['Đức Anh','ĐA',2055],['Phương Vy','PV',1987],['Tuấn Kiệt','TK',1902],['Gia Hân','GH',1811],['Minh Quân','MQ',1740],['Thảo Nguyên','TN',1670],['Ngọc Mai','NM',1625],['Minh Anh','MA',1580],['Hoàng Khang','HK',1525],['Bách','BC',1491],['Hải Nam','HN',1450],['Quốc Long','QL',1422],['Thanh Hà','TH',1390],['Bảo Ngọc','BN',1310],['Anh Tú','AT',1255],['Tuấn Anh','TA',1120],['Thu Trang','TT',980],['Gia Bảo','GB',850],
];
const modes = {
  draft:{ title:'UNO Draft' },
  classic:{ title:'Classic', subtitle:'Cuộc vui nguyên bản.', description:'Bốn người, một người chiến thắng.<br>Đánh hết bài, hô UNO và giành lấy ván đấu!', action:'Chơi Classic', color:'#c1493c', cards:['blue_reverse','yellow_7','red_draw2'] },
  '2v2':{ title:'2 vs 2', subtitle:'Hai người ăn ý, cả đội cùng vui.', description:'Đồng đội ngồi đối diện, luôn nhìn thấy bài nhau.<br>Cùng phối hợp để đưa đội bạn về đích.', action:'Tạo đội 2 vs 2', color:'#32866d', cards:['green_reverse','yellow_7','green_draw2'] },
  side:{ title:'Side to Side', subtitle:'Đồng đội ngay bên cạnh.', description:'Ngồi liền kề, chia sẻ từng nước bài.<br>Một người hết bài, cả đội cùng thắng!', action:'Tạo đội Side to Side', color:'#387dba', cards:['blue_5','yellow_reverse','blue_draw2'] },
  custom:{ title:'Phòng riêng', subtitle:'Cuộc vui theo luật của bạn.', description:'Mời hội bạn, thêm bot và tự chọn luật chơi.<br>Thảnh thơi một ván, không tính điểm ELO.', action:'Tạo phòng riêng', color:'#ab7e22', cards:['wild_wild','green_7','red_reverse'] },
};
const $ = selector => document.querySelector(selector);
const format = n => n.toLocaleString('vi-VN');
const range = rank => rank.max === Infinity ? `${format(rank.min)}+ ELO` : `${format(rank.min)} – ${format(rank.max)} ELO`;
const image = rank => `ranks/${rank.key}.svg`;
let noticeTimer;
function notify(message) {
  $('#notice').textContent=message;
  $('#notice').hidden=false;
  clearTimeout(noticeTimer);
  noticeTimer=setTimeout(()=>$('#notice').hidden=true,3500);
}
function showView(id) {
  if (!['lobby','rank','badges'].includes(id)) id='lobby';
  document.querySelectorAll('.view').forEach(el=>el.hidden=el.id!==id);
  document.body.dataset.view=id;
  document.querySelectorAll('[data-view]').forEach(el=>el.setAttribute('aria-pressed',el.dataset.view===id));
  history.replaceState(null,'',`#${id}`);
}
document.querySelectorAll('[data-view],[data-go]').forEach(button=>button.addEventListener('click',()=>showView(button.dataset.view||button.dataset.go)));
const modeButtons = [...document.querySelectorAll('[data-mode]')];
let activeMode = 1;
const descriptions = {
  classic:'4 người, mỗi người một mình. Ai hết bài trước sẽ thắng.',
  '2v2':'Đồng đội ngồi đối diện. Phối hợp nước bài để cùng chiến thắng.',
  side:'Đồng đội ngồi cạnh nhau. Nhìn thấy bài nhau, cùng nhau về đích.',
  custom:'Mời hội bạn, thêm bot và chọn luật chơi theo cách của bạn.',
  draft:'4 lá ban đầu, 4 vòng chọn bài. Tự chọn bài, tự chọn chiến thuật.',
};
function chooseMode(index) {
  activeMode = (index + modeButtons.length) % modeButtons.length;
  modeButtons.forEach((button,i)=>{
    let offset=(i-activeMode+modeButtons.length)%modeButtons.length;
    if (offset>modeButtons.length/2) offset-=modeButtons.length;
    button.style.setProperty('--offset',offset);
    button.style.setProperty('--distance',Math.abs(offset));
    button.style.zIndex=10-Math.abs(offset);
    button.classList.toggle('selected',i===activeMode);
    button.hidden=Math.abs(offset)>1;
    button.setAttribute('aria-pressed',i===activeMode);
  });
  const key=modeButtons[activeMode].dataset.mode;
  $('#featured-title').textContent=modes[key].title;
  $('#featured-description').textContent=descriptions[key];
  $('#selection-type').textContent=key==='custom'?'PHÒNG RIÊNG':key==='draft'?'CHỌN BÀI ĐẦU VÁN':'XẾP HẠNG';
  $('#play').innerHTML=`${key==='custom'?'TẠO PHÒNG':key==='draft'?'THỬ CHỌN BÀI':'CHƠI NGAY'} <svg><use href="#arrow"/></svg>`;
  document.querySelectorAll('[data-dot]').forEach((el,i)=>el.setAttribute('aria-pressed',i===activeMode));
}
$('.gallery-dots').innerHTML=modeButtons.map((button,i)=>`<button data-dot="${i}" aria-label="Chọn ${modes[button.dataset.mode].title}" aria-pressed="false"></button>`).join('');
document.querySelectorAll('[data-dot]').forEach(el=>el.addEventListener('click',()=>chooseMode(Number(el.dataset.dot))));
modeButtons.forEach((button,i)=>button.addEventListener('click',()=>chooseMode(i)));
$('.previous').addEventListener('click',()=>chooseMode(activeMode-1));
$('.next').addEventListener('click',()=>chooseMode(activeMode+1));
document.addEventListener('keydown',event=>{
  if ($('#lobby').hidden || event.target.closest('input,textarea,select')) return;
  if (event.key==='ArrowLeft' || event.key==='ArrowRight') {
    event.preventDefault();
    chooseMode(activeMode+(event.key==='ArrowRight'?1:-1));
  }
});
chooseMode(activeMode);
function selectRank(key) {
  const rank=ranks.find(r=>r.key===key);
  document.querySelectorAll('[data-tier]').forEach(el=>el.setAttribute('aria-pressed',el.dataset.tier===key));
  $('#rank-large').src=image(rank); $('#rank-large').alt=`Huy hiệu ${rank.name}`;
  $('#rank-title').textContent=rank.name; $('#rank-title').style.color=rank.color;
  $('#rank-range').textContent=range(rank);
  $('#rank-list-title').textContent=`Những người chơi hạng ${rank.name}`;
  const filtered=players.filter(p=>p[2]>=rank.min&&p[2]<=rank.max);
  $('#rank-rows').innerHTML=filtered.map(([name,initials,elo],i)=>`<tr class="${name==='Bách'?'my-row':''}"><td>${String(i+1).padStart(2,'0')}</td><td><div class="player-cell"><span class="avatar ${['mint','coral','sky',''][i%4]}">${initials}</span><span>${name}${name==='Bách'?'<span class="you-tag">Bạn</span>':''}</span></div></td><td><span class="tier-cell"><img src="${image(rank)}" alt="">${rank.name}</span></td><td>${format(elo)}</td></tr>`).join('');
}
$('.tier-tabs').innerHTML=ranks.map(rank=>`<button class="tier-tab" data-tier="${rank.key}" aria-pressed="false"><img src="${image(rank)}" alt=""><b>${rank.name}</b><small>${range(rank)}</small></button>`).join('');
document.querySelectorAll('[data-tier]').forEach(el=>el.addEventListener('click',()=>selectRank(el.dataset.tier)));
$('.badge-grid').innerHTML=ranks.map(rank=>`<article class="badge-item"><img src="${image(rank)}" alt="Huy hiệu ${rank.name}"><h3 style="color:${rank.color}">${rank.name}</h3><p>${range(rank)}</p><small>${rank.shape}</small></article>`).join('');
$('#play').addEventListener('click',()=>{
  if (modeButtons[activeMode].dataset.mode==='draft') location.href='draft.html';
  else notify('Bản duyệt bố cục: thao tác vào trận sẽ được nối khi bạn chốt thiết kế.');
});
document.querySelectorAll('[data-invite]').forEach(el=>el.addEventListener('click',()=>{el.textContent='✓';el.disabled=true;el.setAttribute('aria-label','Đã mời (minh họa)');notify('Đã đổi trạng thái lời mời minh họa.');}));
$('#sound-preview').addEventListener('click',event=>{const el=event.currentTarget;el.setAttribute('aria-pressed',el.getAttribute('aria-pressed')!=='true');notify('Nút âm thanh minh họa; prototype không phát âm thanh.');});
for (const id of ['rules-preview','settings-preview','friends-preview','profile-preview']) $(`#${id}`).addEventListener('click',()=>notify('Bản duyệt tập trung vào sảnh, bảng xếp hạng và bộ huy hiệu.'));
selectRank('gold');
showView(location.hash.slice(1));
