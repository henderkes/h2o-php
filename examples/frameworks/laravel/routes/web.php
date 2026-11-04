<?php
use Illuminate\Support\Facades\Route;
Route::get('/', static fn () => response()->json(['framework' => 'Laravel Octane', 'path' => request()->path(), 'query' => request()->query()]));
Route::get('/hello/{name}', static fn (string $name) => response()->json(['hello' => $name, 'framework' => 'Laravel Octane']));
