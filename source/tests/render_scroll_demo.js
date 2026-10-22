// Preview controls only; the inserted rows use production markup and functions.
(function(){
  var serial=0,last='';
  function append(kind){
    var photo=kind==='photo';
    var source=document.querySelector(photo?'.enil-photo-message':'.enil-message');
    var row=source.cloneNode(true),nodes=row.querySelectorAll('[id]');
    var id='demo-'+(++serial);
    for(var i=0;i<nodes.length;i++)nodes[i].removeAttribute('id');
    row.querySelector('.enil-seg').id='msg-'+id;
    row.className='enil-message '+(kind==='outgoing'?'outgoing':'incoming')+
      (photo?' enil-photo-message':'');
    if(kind==='outgoing'){
      var metadata=row.querySelectorAll('.enil-avatar,.enil-sender');
      for(var j=0;j<metadata.length;j++)metadata[j].parentNode.removeChild(metadata[j]);
    }
    if(!photo)row.querySelector('.enil-text').textContent=
      kind==='outgoing'?'That looks good!':'A new message slides into the conversation.';
    var reactions=row.querySelector('.enil-reactions');
    if(reactions)reactions.parentNode.removeChild(reactions);
    appendMessage(row.outerHTML,kind==='outgoing');last=id;
  }
  window.addEventListener('message',function(event){
    if(event.source!==parent)return;
    var action=event.data;
    if(action==='bottom')enilScrollNow();
    else if(action==='receipt'){if(last)setReadMarker(last);}
    else if(action==='burst'){append('incoming');append('incoming');append('incoming');}
    else if(action==='incoming'||action==='outgoing'||action==='photo')append(action);
  },false);
  window.addEventListener('load',function(){setTimeout(enilScrollNow,100);},false);
})();
