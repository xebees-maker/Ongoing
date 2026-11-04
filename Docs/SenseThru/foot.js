document.querySelectorAll('.s').forEach(function (s, i) {
  if (s.classList.contains('cover')) return;
  var f = document.createElement('div');
  f.className = 'foot';
  f.innerHTML = '<span>' + document.body.dataset.foot + '</span><span>' + (i + 1) + '</span>';
  s.appendChild(f);
});
