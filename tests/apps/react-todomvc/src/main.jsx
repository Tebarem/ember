import { StrictMode, useEffect, useReducer, useState } from 'react';
import { createRoot } from 'react-dom/client';
import '@shared/todo.css';

const STORAGE_KEY = 'todos-react';

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
  const name = window.location.hash.slice(2);
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

function todoReducer(todos, action) {
  switch (action.type) {
    case 'add':
      return [...todos, { id: action.id, title: action.title, completed: false }];
    case 'toggle':
      return todos.map((t) => (t.id === action.id ? { ...t, completed: !t.completed } : t));
    case 'toggleAll':
      return todos.map((t) => ({ ...t, completed: action.completed }));
    case 'destroy':
      return todos.filter((t) => t.id !== action.id);
    case 'rename':
      return todos.map((t) => (t.id === action.id ? { ...t, title: action.title } : t));
    case 'clearCompleted':
      return todos.filter((t) => !t.completed);
    default:
      return todos;
  }
}

function TodoItem({ todo, editing, dispatch, onStartEdit, onStopEdit }) {
  const [draft, setDraft] = useState(todo.title);

  useEffect(() => {
    if (editing) setDraft(todo.title);
  }, [editing, todo.title]);

  const commit = () => {
    const title = draft.trim();
    onStopEdit();
    if (!title) dispatch({ type: 'destroy', id: todo.id });
    else if (title !== todo.title) dispatch({ type: 'rename', id: todo.id, title });
  };

  return (
    <li className={[todo.completed && 'completed', editing && 'editing'].filter(Boolean).join(' ')}>
      <div className="view">
        <input
          className="toggle"
          type="checkbox"
          checked={todo.completed}
          onChange={() => dispatch({ type: 'toggle', id: todo.id })}
        />
        <label onDoubleClick={onStartEdit}>{todo.title}</label>
        <button className="destroy" aria-label="Delete" onClick={() => dispatch({ type: 'destroy', id: todo.id })} />
      </div>
      {editing && (
        <input
          className="edit"
          value={draft}
          autoFocus
          onChange={(e) => setDraft(e.target.value)}
          onBlur={commit}
          onKeyDown={(e) => {
            if (e.key === 'Enter') commit();
            else if (e.key === 'Escape') onStopEdit();
          }}
        />
      )}
    </li>
  );
}

function App() {
  const [todos, dispatch] = useReducer(todoReducer, undefined, loadTodos);
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
    dispatch({ type: 'add', id: `${Date.now()}-${todos.length}`, title });
    setNewTitle('');
  };

  return (
    <section className="todoapp">
      <header className="header">
        <h1>todos</h1>
        <input
          className="new-todo"
          placeholder="What needs to be done?"
          value={newTitle}
          autoFocus
          onChange={(e) => setNewTitle(e.target.value)}
          onKeyDown={(e) => e.key === 'Enter' && addTodo()}
        />
      </header>
      {todos.length > 0 && (
        <section className="main">
          <input
            id="toggle-all"
            className="toggle-all"
            type="checkbox"
            checked={allDone}
            onChange={() => dispatch({ type: 'toggleAll', completed: !allDone })}
          />
          <label htmlFor="toggle-all">Mark all as complete</label>
          <ul className="todo-list">
            {visible.map((todo) => (
              <TodoItem
                key={todo.id}
                todo={todo}
                editing={editingId === todo.id}
                dispatch={dispatch}
                onStartEdit={() => setEditingId(todo.id)}
                onStopEdit={() => setEditingId(null)}
              />
            ))}
          </ul>
        </section>
      )}
      {todos.length > 0 && (
        <footer className="footer">
          <span className="todo-count">
            <strong>{activeCount}</strong> {activeCount === 1 ? 'item' : 'items'} left
          </span>
          <ul className="filters">
            <li><a className={filter === 'all' ? 'selected' : ''} href="#/">All</a></li>
            <li><a className={filter === 'active' ? 'selected' : ''} href="#/active">Active</a></li>
            <li><a className={filter === 'completed' ? 'selected' : ''} href="#/completed">Completed</a></li>
          </ul>
          {todos.length > activeCount ? (
            <button className="clear-completed" onClick={() => dispatch({ type: 'clearCompleted' })}>
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

createRoot(document.getElementById('root')).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
