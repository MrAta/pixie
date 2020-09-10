package k8s

import (
	"context"
	"io/ioutil"
	"strings"
	"time"

	log "github.com/sirupsen/logrus"
	"k8s.io/apimachinery/pkg/api/errors"
	"k8s.io/apimachinery/pkg/runtime"
	"k8s.io/client-go/dynamic"

	"k8s.io/apimachinery/pkg/api/meta"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/cli-runtime/pkg/genericclioptions"
	"k8s.io/cli-runtime/pkg/printers"
	"k8s.io/cli-runtime/pkg/resource"
	"k8s.io/client-go/discovery"
	memory "k8s.io/client-go/discovery/cached"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/rest"
	"k8s.io/client-go/restmapper"
	"k8s.io/client-go/tools/clientcmd"
	cmdutil "k8s.io/kubectl/pkg/cmd/util"
	cmdwait "k8s.io/kubectl/pkg/cmd/wait"
)

// AllResourceKinds has the list of all the resource kinds we use.
var AllResourceKinds = []string{"ClusterRole", "ClusterRoleBinding", "ConfigMap",
	"DaemonSet", "Pod", "Deployment", "Role", "RoleBinding", "Service",
	"ServiceAccount", "Job", "PodSecurityPolicy", "CronJob"}

type restClientAdapter struct {
	clientset  *kubernetes.Clientset
	restConfig *rest.Config
}

func (r *restClientAdapter) ToRESTConfig() (*rest.Config, error) {
	return r.restConfig, nil
}

func (r *restClientAdapter) ToDiscoveryClient() (discovery.CachedDiscoveryInterface, error) {
	return memory.NewMemCacheClient(r.clientset.Discovery()), nil
}

func (r *restClientAdapter) ToRESTMapper() (meta.RESTMapper, error) {
	discoveryClient := r.clientset.Discovery()
	apiGroupResources, err := restmapper.GetAPIGroupResources(discoveryClient)
	if err != nil {
		return nil, err
	}
	return restmapper.NewDiscoveryRESTMapper(apiGroupResources), nil
}

func (r *restClientAdapter) ToRawKubeConfigLoader() clientcmd.ClientConfig {
	log.Fatal("raw kubeconfig loader is not implemented.")
	return nil
}

// ObjectDeleter has methods to delete K8s objects and wait for them. This code is adopted from `kubectl delete`.
type ObjectDeleter struct {
	Namespace     string
	Clientset     *kubernetes.Clientset
	RestConfig    *rest.Config
	Timeout       time.Duration
	dynamicClient dynamic.Interface
}

// DeleteNamespace removes the namespace and all objects within it. Waits for deletion to complete.
func (o *ObjectDeleter) DeleteNamespace() error {
	rca := &restClientAdapter{
		clientset:  o.Clientset,
		restConfig: o.RestConfig,
	}

	f := cmdutil.NewFactory(rca)
	r := f.NewBuilder().
		Unstructured().
		ContinueOnError().
		NamespaceParam(o.Namespace).
		ResourceNames("namespace", o.Namespace).
		RequireObject(false).
		Flatten().
		Do()

	err := r.Err()
	if err != nil {
		return err
	}
	o.dynamicClient, err = f.DynamicClient()
	if err != nil {
		return err
	}

	_, err = o.runDelete(r)
	return err
}

// DeleteByLabel delete objects that match the labels and specified by resourceKinds. Waits for deletion.
func (o *ObjectDeleter) DeleteByLabel(selector string, resourceKinds ...string) (int, error) {
	rca := &restClientAdapter{
		clientset:  o.Clientset,
		restConfig: o.RestConfig,
	}

	f := cmdutil.NewFactory(rca)
	r := f.NewBuilder().
		Unstructured().
		ContinueOnError().
		NamespaceParam(o.Namespace).
		LabelSelector(selector).
		ResourceTypeOrNameArgs(false, strings.Join(resourceKinds, ",")).
		RequireObject(false).
		Flatten().
		Do()

	err := r.Err()
	if err != nil {
		return 0, err
	}
	o.dynamicClient, err = f.DynamicClient()
	if err != nil {
		return 0, err
	}

	return o.runDelete(r)
}

func (o *ObjectDeleter) runDelete(r *resource.Result) (int, error) {
	r = r.IgnoreErrors(errors.IsNotFound)
	deletedInfos := []*resource.Info{}
	uidMap := cmdwait.UIDMap{}
	found := 0
	err := r.Visit(func(info *resource.Info, err error) error {
		if err != nil {
			return err
		}
		deletedInfos = append(deletedInfos, info)
		found++

		options := &metav1.DeleteOptions{}
		policy := metav1.DeletePropagationBackground
		options.PropagationPolicy = &policy

		response, err := o.deleteResource(info, options)
		if err != nil {
			return err
		}
		resourceLocation := cmdwait.ResourceLocation{
			GroupResource: info.Mapping.Resource.GroupResource(),
			Namespace:     info.Namespace,
			Name:          info.Name,
		}
		if status, ok := response.(*metav1.Status); ok && status.Details != nil {
			uidMap[resourceLocation] = status.Details.UID
			return nil
		}
		responseMetadata, err := meta.Accessor(response)
		if err != nil {
			// We don't have UID, but we didn't fail the delete, next best thing is just skipping the UID.
			log.WithError(err).Trace("missing UID")
			return nil
		}
		uidMap[resourceLocation] = responseMetadata.GetUID()

		return nil
	})
	if err != nil {
		return 0, err
	}
	if found == 0 {
		return 0, nil
	}

	effectiveTimeout := o.Timeout
	if effectiveTimeout == 0 {
		// if we requested to wait forever, set it to a week.
		effectiveTimeout = 168 * time.Hour
	}
	waitOptions := cmdwait.WaitOptions{
		ResourceFinder: genericclioptions.ResourceFinderForResult(resource.InfoListVisitor(deletedInfos)),
		UIDMap:         uidMap,
		DynamicClient:  o.dynamicClient,
		Timeout:        effectiveTimeout,

		Printer:     printers.NewDiscardingPrinter(),
		ConditionFn: cmdwait.IsDeleted,
		IOStreams: genericclioptions.IOStreams{
			Out:    ioutil.Discard,
			ErrOut: ioutil.Discard,
		},
	}
	return found, waitOptions.RunWait()
}

func (o *ObjectDeleter) deleteResource(info *resource.Info, deleteOptions *metav1.DeleteOptions) (runtime.Object, error) {
	deleteResponse, err := resource.
		NewHelper(info.Client, info.Mapping).
		DeleteWithOptions(info.Namespace, info.Name, deleteOptions)

	if err != nil {
		return nil, cmdutil.AddSourceToErr("deleting", info.Source, err)
	}

	return deleteResponse, nil
}

// DeleteClusterRole deletes the clusterrole with the given name.
func DeleteClusterRole(clientset *kubernetes.Clientset, name string) error {
	crs := clientset.RbacV1().ClusterRoles()
	err := crs.Delete(context.Background(), name, metav1.DeleteOptions{})
	if err != nil {
		return err
	}

	return nil
}

// DeleteClusterRoleBinding deletes the clusterrolebinding with the given name.
func DeleteClusterRoleBinding(clientset *kubernetes.Clientset, name string) error {
	crbs := clientset.RbacV1().ClusterRoleBindings()

	err := crbs.Delete(context.Background(), name, metav1.DeleteOptions{})
	if err != nil {
		return err
	}

	return nil
}

// DeleteConfigMap deletes the config map in the namespace with the given name.
func DeleteConfigMap(clientset *kubernetes.Clientset, name string, namespace string) error {
	cm := clientset.CoreV1().ConfigMaps(namespace)

	err := cm.Delete(context.Background(), name, metav1.DeleteOptions{})
	if err != nil {
		return err
	}

	return nil
}

// DeleteAllResources deletes all resources in the given namespace with the given selector.
func DeleteAllResources(clientset *kubernetes.Clientset, ns string, selectors string) error {
	err := DeleteDeployments(clientset, ns, selectors)
	if err != nil {
		return err
	}

	err = DeleteDaemonSets(clientset, ns, selectors)
	if err != nil {
		return err
	}

	err = DeleteServices(clientset, ns, selectors)
	if err != nil {
		return err
	}

	err = DeletePods(clientset, ns, selectors)
	if err != nil {
		return err
	}

	return nil
}

// DeleteDeployments deletes all deployments in the namespace with the given selector.
func DeleteDeployments(clientset *kubernetes.Clientset, namespace string, selectors string) error {
	deployments := clientset.AppsV1().Deployments(namespace)

	if err := deployments.DeleteCollection(context.Background(), metav1.DeleteOptions{}, metav1.ListOptions{LabelSelector: selectors}); err != nil {
		return err
	}
	return nil
}

// DeleteDaemonSets deletes all daemonsets in the namespace with the given selector.
func DeleteDaemonSets(clientset *kubernetes.Clientset, namespace string, selectors string) error {
	daemonsets := clientset.AppsV1().DaemonSets(namespace)

	if err := daemonsets.DeleteCollection(context.Background(), metav1.DeleteOptions{}, metav1.ListOptions{LabelSelector: selectors}); err != nil {
		return err
	}
	return nil
}

// DeleteServices deletes all services in the namespace with the given selector.
func DeleteServices(clientset *kubernetes.Clientset, namespace string, selectors string) error {
	svcs := clientset.CoreV1().Services(namespace)

	l, err := svcs.List(context.Background(), metav1.ListOptions{LabelSelector: selectors})
	if err != nil {
		return err
	}
	for _, s := range l.Items {
		err = svcs.Delete(context.Background(), s.ObjectMeta.Name, metav1.DeleteOptions{})
		if err != nil {
			return err
		}
	}

	return nil
}

// DeletePods deletes all pods in the namespace with the given selector.
func DeletePods(clientset *kubernetes.Clientset, namespace string, selectors string) error {
	pods := clientset.CoreV1().Pods(namespace)

	l, err := pods.List(context.Background(), metav1.ListOptions{LabelSelector: selectors})
	if err != nil {
		return err
	}
	for _, s := range l.Items {
		err = pods.Delete(context.Background(), s.ObjectMeta.Name, metav1.DeleteOptions{})
		if err != nil {
			return err
		}
	}

	return nil
}
