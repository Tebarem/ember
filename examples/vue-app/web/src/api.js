// The native API of the host. Relative URLs resolve against app://local/index.html, so
// 'api/notes' is app://local/api/notes, which main.c answers (no server, no network).
async function call(path, init) {
  const res = await fetch(path, init);
  if (!res.ok) throw new Error(`${path}: ${res.status} ${await res.text()}`);
  return res.status === 204 ? null : res.json();
}

export const getSystem = () => call('api/system');
export const getNotes = () => call('api/notes');
export const addNote = (text) =>
  call('api/notes', { method: 'POST', headers: { 'Content-Type': 'text/plain' }, body: text });
export const deleteNote = (id) => call(`api/notes/${id}`, { method: 'DELETE' });
