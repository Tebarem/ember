import { render } from 'preact';
import { useEffect, useState } from 'preact/hooks';
import '@shared/todo.css';

const STORAGE_KEY = 'todos-preact';
let lastId = 0;

function loadTodos() {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    return raw ? JSON.parse(raw) : [];
  } catch {
    return [];
  }
}

function saveTodos(todos) {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(todos));
  } catch {
    // Storage can be unavailable; the list still works in memory.
  }
}

function filterFromHash() {
  const name = location.hash.slice(2);
  return name === 'active' || name === 'completed' ? name : 'all';
}

function useHashFilter() {
  const [filter, setFilter] = useState(filterFromHash);
  useEffect(() => {
    const onHash = () => setFilter(filterFromHash());
    window.addEventListener('hashchange', onHash);
    return () => window.removeEventListener('hashchange', onHash);
  }, []);
  return filter;
}

function TodoItem({ todo, editing, onToggle, onDestroy, onStartEdit, onCommit, onCancel }) {
  const [draft, setDraft] = useState(todo.title);
  const classes = [todo.completed ? 'completed' : '', editing ? 'editing' : ''].join(' ').trim();

  useEffect(() => {
    if (editing) setDraft(todo.title);
  }, [editing]);

  return (
    <li class={classes}>
      <div class="view">
        <input class="toggle" type="checkbox" checked={todo.completed} onChange={onToggle} />
        <label onDblClick={onStartEdit}>{todo.title}</label>
        <button class="destroy" onClick={onDestroy} aria-label="Delete" />
      </div>
      {editing && (
        <input
          class="edit"
          value={draft}
          ref={(el) => el && document.activeElement !== el && el.focus()}
          onInput={(e) => setDraft(e.currentTarget.value)}
          onBlur={() => onCommit(draft)}
          onKeyDown={(e) => {
            if (e.key === 'Enter') onCommit(draft);
            else if (e.key === 'Escape') onCancel();
          }}
        />
      )}
    </li>
  );
}

function App() {
  const [todos, setTodos] = useState(loadTodos);
  const [newTitle, setNewTitle] = useState('');
  const [editingId, setEditingId] = useState(null);
  const filter = useHashFilter();

  useEffect(() => saveTodos(todos), [todos]);

  const visible = todos.filter((t) =>
    filter === 'all' ? true : filter === 'active' ? !t.completed : t.completed,
  );
  const activeCount = todos.filter((t) => !t.completed).length;
  const allDone = todos.length > 0 && activeCount === 0;

  const addTodo = () => {
    const title = newTitle.trim();
    if (!title) return;
    lastId += 1;
    setTodos([...todos, { id: `${Date.now()}-${lastId}`, title, completed: false }]);
    setNewTitle('');
  };

  const commit = (id, text) => {
    const title = text.trim();
    setEditingId(null);
    if (!title) setTodos(todos.filter((t) => t.id !== id));
    else setTodos(todos.map((t) => (t.id === id ? { ...t, title } : t)));
  };

  return (
    <section class="todoapp">
      <header class="header">
        <h1>todos</h1>
        <input
          class="new-todo"
          placeholder="What needs to be done?"
          value={newTitle}
          autoFocus
          onInput={(e) => setNewTitle(e.currentTarget.value)}
          onKeyDown={(e) => e.key === 'Enter' && addTodo()}
        />
      </header>
      {todos.length > 0 && (
        <section class="main">
          <input
            id="toggle-all"
            class="toggle-all"
            type="checkbox"
            checked={allDone}
            onChange={() => setTodos(todos.map((t) => ({ ...t, completed: !allDone })))}
          />
          <label for="toggle-all">Mark all as complete</label>
          <ul class="todo-list">
            {visible.map((todo) => (
              <TodoItem
                key={todo.id}
                todo={todo}
                editing={editingId === todo.id}
                onToggle={() =>
                  setTodos(todos.map((t) => (t.id === todo.id ? { ...t, completed: !t.completed } : t)))
                }
                onDestroy={() => setTodos(todos.filter((t) => t.id !== todo.id))}
                onStartEdit={() => setEditingId(todo.id)}
                onCommit={(text) => commit(todo.id, text)}
                onCancel={() => setEditingId(null)}
              />
            ))}
          </ul>
        </section>
      )}
      {todos.length > 0 && (
        <footer class="footer">
          <span class="todo-count">
            <strong>{activeCount}</strong> {activeCount === 1 ? 'item' : 'items'} left
          </span>
          <ul class="filters">
            <li><a class={filter === 'all' ? 'selected' : ''} href="#/">All</a></li>
            <li><a class={filter === 'active' ? 'selected' : ''} href="#/active">Active</a></li>
            <li><a class={filter === 'completed' ? 'selected' : ''} href="#/completed">Completed</a></li>
          </ul>
          {todos.length > activeCount ? (
            <button class="clear-completed" onClick={() => setTodos(todos.filter((t) => !t.completed))}>
              Clear completed
            </button>
          ) : (
            <span />
          )}
        </footer>
      )}
    </section>
  );
}

render(<App />, document.getElementById('app'));
