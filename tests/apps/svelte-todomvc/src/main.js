import { mount } from 'svelte';
import '@shared/todo.css';
import App from './App.svelte';

mount(App, { target: document.getElementById('app') });
