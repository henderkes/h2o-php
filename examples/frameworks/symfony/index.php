<?php
// Symfony's own unmodified runtime selects its FrankenPhpWorkerRunner.
use Symfony\Component\EventDispatcher\EventDispatcher;
use Symfony\Component\HttpFoundation\JsonResponse;
use Symfony\Component\HttpFoundation\RequestStack;
use Symfony\Component\HttpKernel\Controller\ArgumentResolver;
use Symfony\Component\HttpKernel\Controller\ControllerResolver;
use Symfony\Component\HttpKernel\Event\RequestEvent;
use Symfony\Component\HttpKernel\HttpKernel;
use Symfony\Component\HttpKernel\KernelEvents;

require_once __DIR__.'/vendor/autoload_runtime.php';
return static function (array $context): HttpKernel {
    $boot = bin2hex(random_bytes(8));
    $dispatcher = new EventDispatcher();
    $dispatcher->addListener(KernelEvents::REQUEST, static function (RequestEvent $event) use ($boot): void {
        $event->getRequest()->attributes->set('_controller', static fn () => new JsonResponse([
            'framework' => 'Symfony', 'boot' => $boot,
            'path' => $event->getRequest()->getPathInfo(), 'query' => $event->getRequest()->query->all(),
        ]));
    });
    return new HttpKernel($dispatcher, new ControllerResolver(), new RequestStack(), new ArgumentResolver());
};
