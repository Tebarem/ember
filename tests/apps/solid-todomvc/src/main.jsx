import { createEffect, createSignal, For, Show } from 'solid-js';
import { render } from 'solid-js/web';
import '@shared/todo.css';

const STORAGE_KEY = 'todos-solid';
let seq = 0;

function loadTodos() {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    return raw ? JSON.parse(raw) : [];
  } catch {
    return [];
  }
}

function filterFromHash() {
  const name = location.hash.slice(2);
  return name === 'active' || name === 'completed' ? name : 'all';
}

function App() {
  const [todos, setTodos] = createSignal(loadTodos());
  const [newTitle, setNewTitle] = createSignal('');
  const [editingId, setEditingId] = createSignal(null);
  const [editTitle, setEditTitle] = createSignal('');
  const [filter, setFilter] = createSignal(filterFromHash());

  createEffect(() => {
    const snapshot = JSON.stringify(todos());
    try {
      localStorage.setItem(STORAGE_KEY, snapshot);
    } catch {
      // Storage can be unavailable; the list still works in memory.
    }
  });

  window.addEventListener('hashchange', () => setFilter(filterFromHash()));

  const visible = () =>
    todos().filter((t) => (filter() === 'all' ? true : filter() === 'active' ? !t.completed : t.completed));
  const activeCount = () => todos().filter((t) => !t.completed).length;
  const allDone = () => todos().length > 0 && activeCount() === 0;

  const update = (id, patch) => setTodos(todos().map((t) => (t.id === id ? { ...t, ...patch } : t)));
  const remove = (id) => setTodos(todos().filter((t) => t.id !== id));

  const addTodo = (e) => {
    if (e.key !== 'Enter') return;
    const title = newTitle().trim();
    if (!title) return;
    seq += 1;
    setTodos([...todos(), { id: `${Date.now()}-${seq}`, title, completed: false }]);
    setNewTitle('');
  };

  const commitEdit = (id) => {
    if (editingId() !== id) return;
    const title = editTitle().trim();
    setEditingId(null);
    if (!title) remove(id);
    else update(id, { title });
  };

  const startEdit = (todo) => {
    setEditingId(todo.id);
    setEditTitle(todo.title);
  };

  return (
    <section class="todoapp">
      <header class="header">
        <h1>todos</h1>
        <input
          class="new-todo"
          placeholder="What needs to be done?"
          autofocus
          value={newTitle()}
          onInput={(e) => setNewTitle(e.currentTarget.value)}
          onKeyDown={addTodo}
        />
      </header>
      <Show when={todos().length > 0}>
        <section class="main">
          <input
            id="toggle-all"
            class="toggle-all"
            type="checkbox"
            checked={allDone()}
            onChange={() => setTodos(todos().map((t) => ({ ...t, completed: !allDone() })))}
          />
          <label for="toggle-all">Mark all as complete</label>
          <ul class="todo-list">
            <For each={visible()}>
              {(todo) => (
                <li classList={{ completed: todo.completed, editing: editingId() === todo.id }}>
                  <div class="view">
                    <input
                      class="toggle"
                      type="checkbox"
                      checked={todo.completed}
                      onChange={() => update(todo.id, { completed: !todo.completed })}
                    />
                    <label onDblClick={() => startEdit(todo)}>{todo.title}</label>
                    <button class="destroy" aria-label="Delete" onClick={() => remove(todo.id)} />
                  </div>
                  <Show when={editingId() === todo.id}>
                    <input
                      class="edit"
                      value={editTitle()}
                      ref={(el) => queueMicrotask(() => el.focus())}
                      onInput={(e) => setEditTitle(e.currentTarget.value)}
                      onBlur={() => commitEdit(todo.id)}
                      onKeyDown={(e) => {
                        if (e.key === 'Enter') commitEdit(todo.id);
                        else if (e.key === 'Escape') setEditingId(null);
                      }}
                    />
                  </Show>
                </li>
              )}
            </For>
          </ul>
        </section>
        <footer class="footer">
          <span class="todo-count">
            <strong>{activeCount()}</strong> {activeCount() === 1 ? 'item' : 'items'} left
          </span>
          <ul class="filters">
            <li><a classList={{ selected: filter() === 'all' }} href="#/">All</a></li>
            <li><a classList={{ selected: filter() === 'active' }} href="#/active">Active</a></li>
            <li><a classList={{ selected: filter() === 'completed' }} href="#/completed">Completed</a></li>
          </ul>
          <Show when={todos().length > activeCount()} fallback={<span />}>
            <button class="clear-completed" onClick={() => setTodos(todos().filter((t) => !t.completed))}>
              Clear completed
            </button>
          </Show>
        </footer>
      </Show>
    </section>
  );
}

render(() => <App />, document.getElementById('app'));
